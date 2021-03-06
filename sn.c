/* Supernode for n2n-2.x */

/* (c) 2009 Richard Andrews <andrews@ntop.org> 
 *
 * Contributions by:
 *    Lukasz Taczuk
 *    Struan Bartlett
 */


#include "n2n.h"

#ifdef N2N_MULTIPLE_SUPERNODES
#include "sn_multiple.h"
#endif

#define N2N_SN_LPORT_DEFAULT 7654
#define N2N_SN_PKTBUF_SIZE   2048

#define N2N_SN_MGMT_PORT                5645


struct sn_stats
{
    size_t errors;              /* Number of errors encountered. */
    size_t reg_super;           /* Number of REGISTER_SUPER requests received. */
    size_t reg_super_nak;       /* Number of REGISTER_SUPER requests declined. */
    size_t fwd;                 /* Number of messages forwarded. */
    size_t broadcast;           /* Number of messages broadcast to a community. */
    time_t last_fwd;            /* Time when last message was forwarded. */
    time_t last_reg_super;      /* Time when last REGISTER_SUPER was received. */
};

typedef struct sn_stats sn_stats_t;

struct n2n_sn
{
    time_t              start_time;     /* Used to measure uptime. */
    sn_stats_t          stats;
    int                 daemon;         /* If non-zero then daemonise. */
    uint16_t            lport;          /* Local UDP port to bind to. */
    int                 sock;           /* Main socket for UDP traffic with edges. */
    int                 mgmt_sock;      /* management socket. */
#ifdef N2N_MULTIPLE_SUPERNODES
    uint8_t             snm_discovery_state;
    int                 sn_port;
    int                 sn_sock;        /* multiple supernodes socket */
    unsigned int        seq_num;        /* sequence number for SN communication */
    sn_list_t           supernodes;
    comm_list_t         communities;
#endif
    struct n2n_list     edges;          /* Link list of registered edges. */
};

typedef struct n2n_sn n2n_sn_t;


static int try_forward(n2n_sn_t *sss,
                       const n2n_common_t *cmn,
                       const n2n_mac_t dstMac,
                       const uint8_t *pktbuf,
                       size_t pktsize);

static int try_broadcast(n2n_sn_t *sss,
                         const n2n_common_t *cmn,
                         const n2n_mac_t srcMac,
                         const uint8_t *pktbuf,
                         size_t pktsize);



/** Initialise the supernode structure */
static int init_sn(n2n_sn_t *sss)
{
#ifdef WIN32
    initWin32();
#endif
    memset(sss, 0, sizeof(n2n_sn_t));

    sss->daemon = 1; /* By defult run as a daemon. */
    sss->lport = N2N_SN_LPORT_DEFAULT;
    sss->sock = -1;
    sss->mgmt_sock = -1;
    list_init(&sss->edges);

#ifdef N2N_MULTIPLE_SUPERNODES
    sss->snm_discovery_state = N2N_SNM_STATE_DISCOVERY;
    sss->sn_sock = -1;
    sss->seq_num = -1;
    memset(&sss->supernodes, 0, sizeof(sn_list_t));
    memset(&sss->communities, 0, sizeof(comm_list_t));
#endif

    return 0; /* OK */
}

/** Deinitialise the supernode structure and deallocate any memory owned by
 *  it. */
static void deinit_sn(n2n_sn_t *sss)
{
    if (sss->sock >= 0)
    {
        closesocket(sss->sock);
    }
    sss->sock = -1;

    if (sss->mgmt_sock >= 0)
    {
        closesocket(sss->mgmt_sock);
    }
    sss->mgmt_sock = -1;

    purge_peer_list(&(sss->edges), 0xffffffff);

#ifdef N2N_MULTIPLE_SUPERNODES
    if (sss->sn_sock)
    {
        closesocket(sss->sn_sock);
    }
    sss->seq_num = -1;
    list_clear(&sss->supernodes.head);
    list_clear(&sss->communities.head);
#endif
}


/** Determine the appropriate lifetime for new registrations.
 *
 *  If the supernode has been put into a pre-shutdown phase then this lifetime
 *  should not allow registrations to continue beyond the shutdown point.
 */
static uint16_t reg_lifetime(n2n_sn_t *sss)
{
    return 120;
}


/** Update the edge table with the details of the edge which contacted the
 *  supernode. */
static int update_edge(n2n_sn_t *sss,
                       const n2n_mac_t edgeMac,
                       const n2n_community_t community,
                       const n2n_sock_t *sender_sock,
                       time_t now)
{
    macstr_t            mac_buf;
    n2n_sock_str_t      sockbuf;
    struct peer_info   *scan;

    traceDebug("update_edge for %s [%s]",
               macaddr_str(mac_buf, edgeMac),
               sock_to_cstr(sockbuf, sender_sock));

    scan = find_peer_by_mac(&sss->edges, edgeMac);

    if (NULL == scan)
    {
        /* Not known */

        scan = (struct peer_info *) calloc(1, sizeof(struct peer_info)); /* deallocated in purge_expired_registrations */

        memcpy(scan->community_name, community, sizeof(n2n_community_t));
        memcpy(&(scan->mac_addr), edgeMac, sizeof(n2n_mac_t));
        memcpy(&(scan->sock), sender_sock, sizeof(n2n_sock_t));

        /* insert this guy at the head of the edges list */
        list_add(&sss->edges, &scan->list);

        traceInfo("update_edge created   %s ==> %s",
                   macaddr_str(mac_buf, edgeMac),
                   sock_to_cstr(sockbuf, sender_sock));
    }
    else
    {
        /* Known */
        if ((0 != memcmp(community, scan->community_name, sizeof(n2n_community_t))) ||
            (0 != sock_equal(sender_sock, &(scan->sock))))
        {
            memcpy(scan->community_name, community, sizeof(n2n_community_t));
            memcpy(&(scan->sock), sender_sock, sizeof(n2n_sock_t));

            traceInfo("update_edge updated   %s ==> %s",
                       macaddr_str(mac_buf, edgeMac),
                       sock_to_cstr(sockbuf, sender_sock));
        }
        else
        {
            traceDebug("update_edge unchanged %s ==> %s",
                       macaddr_str(mac_buf, edgeMac),
                       sock_to_cstr(sockbuf, sender_sock));
        }

    }

    scan->last_seen = now;
    return 0;
}



/** Try to forward a message to a unicast MAC. If the MAC is unknown then
 *  broadcast to all edges in the destination community.
 */
static int try_forward(n2n_sn_t *sss,
                       const n2n_common_t *cmn,
                       const n2n_mac_t dstMac,
                       const uint8_t *pktbuf,
                       size_t pktsize)
{
    struct peer_info   *scan;
    macstr_t            mac_buf;
    n2n_sock_str_t      sockbuf;

    scan = find_peer_by_mac(&sss->edges, dstMac);

    if (NULL != scan)
    {
        int data_sent_len;
        data_sent_len = sendto_sock(sss->sock, pktbuf, pktsize, &scan->sock);

        if (data_sent_len == pktsize)
        {
            ++(sss->stats.fwd);
            traceDebug("unicast %lu to [%s] %s",
                       pktsize,
                       sock_to_cstr(sockbuf, &(scan->sock)),
                       macaddr_str(mac_buf, scan->mac_addr));
        }
        else
        {
            ++(sss->stats.errors);
            traceError("unicast %lu to [%s] %s FAILED (%d: %s)",
                       pktsize,
                       sock_to_cstr(sockbuf, &(scan->sock)),
                       macaddr_str(mac_buf, scan->mac_addr),
                       errno, strerror(errno));
        }
    }
    else
    {
        traceDebug("try_forward unknown MAC");

        /* Not a known MAC so drop. */
    }
    
    return 0;
}


/** Try and broadcast a message to all edges in the community.
 *
 *  This will send the exact same datagram to zero or more edges registered to
 *  the supernode.
 */
static int try_broadcast(n2n_sn_t *sss,
                         const n2n_common_t *cmn,
                         const n2n_mac_t srcMac,
                         const uint8_t *pktbuf,
                         size_t pktsize)
{
    struct peer_info   *scan;
    macstr_t            mac_buf;
    n2n_sock_str_t      sockbuf;

    traceDebug("try_broadcast");

    N2N_LIST_FOR_EACH_ENTRY(scan, &sss->edges)
    {
        if ((0 == memcmp(scan->community_name, cmn->community, sizeof(n2n_community_t))) &&
            (0 != memcmp(srcMac, scan->mac_addr, sizeof(n2n_mac_t))))
        /* REVISIT: exclude if the destination socket is where the packet came from. */
        {
            int data_sent_len;
            data_sent_len = sendto_sock(sss->sock, pktbuf, pktsize, &scan->sock);

            if (data_sent_len != pktsize)
            {
                ++(sss->stats.errors);
                traceWarning("multicast %lu to [%s] %s failed %s",
                           pktsize,
                           sock_to_cstr(sockbuf, &(scan->sock)),
                           macaddr_str(mac_buf, scan->mac_addr),
                           strerror(errno));
            }
            else
            {
                ++(sss->stats.broadcast);
                traceDebug("multicast %lu to [%s] %s",
                           pktsize,
                           sock_to_cstr(sockbuf, &(scan->sock)),
                           macaddr_str(mac_buf, scan->mac_addr));
            }
        }
    } /* loop */
    
    return 0;
}


static int process_mgmt(n2n_sn_t *sss,
                        const struct sockaddr_in *sender_sock,
                        const uint8_t *mgmt_buf,
                        size_t mgmt_size,
                        time_t now)
{
    char resbuf[N2N_SN_PKTBUF_SIZE];
    size_t ressize = 0;
    ssize_t r;

    traceDebug("process_mgmt");

    ressize += snprintf(resbuf + ressize, N2N_SN_PKTBUF_SIZE - ressize,
                        "----------------\n");

    ressize += snprintf(resbuf + ressize, N2N_SN_PKTBUF_SIZE - ressize,
                        "uptime    %lu\n", (now - sss->start_time));

    ressize += snprintf(resbuf + ressize, N2N_SN_PKTBUF_SIZE - ressize,
                        "edges     %u\n",
                        (unsigned int) list_size(&sss->edges));

    ressize += snprintf(resbuf + ressize, N2N_SN_PKTBUF_SIZE - ressize,
                        "errors    %u\n",
                        (unsigned int) sss->stats.errors);

    ressize += snprintf(resbuf + ressize, N2N_SN_PKTBUF_SIZE - ressize,
                        "reg_sup   %u\n",
                        (unsigned int) sss->stats.reg_super);

    ressize += snprintf(resbuf + ressize, N2N_SN_PKTBUF_SIZE - ressize,
                        "reg_nak   %u\n",
                        (unsigned int) sss->stats.reg_super_nak);

    ressize += snprintf(resbuf + ressize, N2N_SN_PKTBUF_SIZE - ressize,
                        "fwd       %u\n",
                        (unsigned int) sss->stats.fwd);

    ressize += snprintf(resbuf + ressize, N2N_SN_PKTBUF_SIZE - ressize,
                        "broadcast %u\n",
                        (unsigned int) sss->stats.broadcast);

    ressize += snprintf(resbuf + ressize, N2N_SN_PKTBUF_SIZE - ressize,
                        "last fwd  %lu sec ago\n",
                        (long unsigned int) (now - sss->stats.last_fwd));

    ressize += snprintf(resbuf + ressize, N2N_SN_PKTBUF_SIZE - ressize,
                        "last reg  %lu sec ago\n",
                        (long unsigned int) (now - sss->stats.last_reg_super));


    r = sendto(sss->mgmt_sock, resbuf, ressize, 0/*flags*/,
               (struct sockaddr *) sender_sock, sizeof(struct sockaddr_in));

    if (r <= 0)
    {
        ++(sss->stats.errors);
        traceError("process_mgmt : sendto failed. %s", strerror(errno));
    }

    return 0;
}

#ifdef N2N_MULTIPLE_SUPERNODES

static int load_snm_info(n2n_sn_t *sss)
{
    int new_ones = 0;
    struct sn_info *sni = NULL;

    /* load supernodes */
    struct n2n_list cmdline_supernodes  = sss->supernodes.head;
    list_init(&sss->supernodes.head);

    sprintf(sss->supernodes.filename, "SN_SNM_%d", sss->sn_port);
    if (read_supernodes_from_file( sss->supernodes.filename,
                                  &sss->supernodes.head))
    {
        traceError("Failed to open supernodes file. %s", strerror(errno));
        return -1;
    }

    /* check if we had some new supernodes before reading from file */

    N2N_LIST_FOR_EACH_ENTRY(sni, &cmdline_supernodes)
    {
        new_ones += update_supernodes(&sss->supernodes, &sni->sn);
    }
    list_clear(&cmdline_supernodes);

    if (new_ones)
    {
        write_supernodes_to_file( sss->supernodes.filename,
                                 &sss->supernodes.head);
    }

    /* load communities */
    sprintf(sss->communities.filename, "SN_COMM_%d", sss->sn_port);
    if (read_communities_from_file( sss->communities.filename,
                                   &sss->communities.persist))
    {
        traceError("Failed to open communities file. %s", strerror(errno));
        return -1;
    }

    if (list_empty(&sss->supernodes.head))    /* first running supernode */
    {
        sss->snm_discovery_state = N2N_SNM_STATE_READY;
    }

    return 0;
}


static void send_snm_adv(n2n_sn_t *sss, n2n_sock_t *sn, struct n2n_list *comm_list);

static void advertise_all(n2n_sn_t *sss)
{
    struct sn_info *sni = NULL;

    if (list_empty(&sss->communities.head))
        return;

    N2N_LIST_FOR_EACH_ENTRY(sni, &sss->supernodes.head)
    {
        send_snm_adv(sss, &sni->sn, &sss->communities.head);
    }
}

static void advertise_community_to_all(n2n_sn_t *sss, const n2n_community_t community)
{
    struct sn_info *sni = NULL;
    struct comm_info ci;
    struct n2n_list  tmp_list;

    memset(&ci, 0, sizeof(struct comm_info));
    memcpy(ci.name, community, sizeof(n2n_community_t));
    list_add(&tmp_list, &ci.list);

    N2N_LIST_FOR_EACH_ENTRY(sni, &sss->supernodes.head)
    {
        send_snm_adv(sss, &sni->sn, &tmp_list);
    }
}

static void communities_discovery(n2n_sn_t *sss, time_t nowTime)
{
    if (nowTime - sss->start_time < N2N_SUPER_DISCOVERY_INTERVAL)
    {
        return;
    }

    if (sss->snm_discovery_state == N2N_SNM_STATE_DISCOVERY)
    {
        struct n2n_list tmp_list = sss->communities.head;   /* queried communities */
        int comm_num = list_size(&sss->communities.persist);

        /* add new communities */
        struct comm_info *ci = N2N_LIST_FIRST_ENTRY(&tmp_list, struct comm_info);

        sss->communities.head = sss->communities.persist;

        while (comm_num < N2N_MAX_COMM_PER_SN && ci != NULL)
        {
            if (ci->sn_num < N2N_MIN_SN_PER_COMM)
            {
                /* add new community without setting supernodes */
                if (add_new_community(&sss->communities, ci->name, NULL))
                    comm_num++;
            }

            ci = N2N_LIST_NEXT_ENTRY(ci);
        }
        list_clear(&tmp_list);

        /* send ADV to all */
        advertise_all(sss);

        sss->snm_discovery_state = N2N_SNM_STATE_READY;
    }
}

static void send_snm_req(n2n_sn_t         *sss,
                         n2n_sock_t       *sn,
                         int               req_communities,
                         snm_comm_name_t  *communities,
                         int               communities_num)
{
    uint8_t pktbuf[N2N_PKT_BUF_SIZE];
    size_t idx;
    n2n_sock_str_t sockbuf;
    snm_hdr_t hdr = { SNM_TYPE_REQ_LIST_MSG, 0, ++sss->seq_num };
    n2n_SNM_REQ_t req = { 0, NULL };

    if (sn_is_loopback(sn, sss->sn_port))
        return;

    SET_S(hdr.flags);

    if (req_communities)
    {
        SET_C(hdr.flags);
    }
    else if (communities)
    {
        SET_N(hdr.flags);
        req.comm_num = communities_num & 0xFFFF;
        req.comm_ptr = communities;
    }

    idx = 0;
    encode_SNM_REQ(pktbuf, &idx, &hdr, &req);

    traceInfo("send SNM_REQ to %s", sock_to_cstr(sockbuf, sn));

    sendto_sock(sss->sn_sock, sn, pktbuf, idx);
}

static void send_req_to_all_supernodes(n2n_sn_t         *sss,
                                       int               req_communities,
                                       snm_comm_name_t  *communities,
                                       int               communities_num)
{
    struct sn_info *sni = NULL;

    N2N_LIST_FOR_EACH_ENTRY(sni, &sss->supernodes.head)
    {
        /* check what's new */
        send_snm_req(sss, &sni->sn, req_communities, communities, communities_num);
    }
}

static void send_snm_rsp(n2n_sn_t *sss, const n2n_sock_t *sock, snm_hdr_t *hdr, n2n_SNM_REQ_t *req)
{
    n2n_sock_str_t sock_buf;
    uint8_t pktbuf[N2N_PKT_BUF_SIZE];
    size_t  idx;
    snm_hdr_t rsp_hdr;
    n2n_SNM_INFO_t rsp;

    build_snm_info(sss->sock, &sss->supernodes, &sss->communities, hdr, req, &rsp_hdr, &rsp);

    idx = 0;
    encode_SNM_INFO(pktbuf, &idx, &rsp_hdr, &rsp);

    traceInfo("send SNM_RSP to %s", sock_to_cstr(sock_buf, sock));
    log_SNM_INFO(&rsp);

    clear_snm_info(&rsp);

    sendto_sock(sss->sn_sock, sock, pktbuf, idx);
}

static void send_snm_adv(n2n_sn_t *sss, n2n_sock_t *sn, struct n2n_list *comm_list)
{
    uint8_t pktbuf[N2N_PKT_BUF_SIZE];
    size_t idx;
    n2n_sock_str_t sockbuf;
    snm_hdr_t hdr;
    n2n_SNM_ADV_t adv;

    if (sn_is_loopback(sn, sss->sn_port))
        return;

    build_snm_adv(sss->sock, comm_list, &hdr, &adv);

    if (sss->snm_discovery_state != N2N_SNM_STATE_READY)
    {
        SET_A(hdr.flags);
    }

    idx = 0;
    encode_SNM_ADV(pktbuf, &idx, &hdr, &adv);

    traceInfo("send ADV to %s", sock_to_cstr(sockbuf, sn));
    log_SNM_ADV(&adv);

    sendto_sock(sss->sn_sock, sn, pktbuf, idx);
}

static int process_sn_msg(n2n_sn_t *sss,
                          const struct sockaddr_in *sender_sock,
                          const uint8_t *msg_buf,
                          size_t msg_size,
                          time_t now)
{
    snm_hdr_t           hdr; /* common fields in the packet header */
    size_t              rem;
    size_t              idx;
    size_t              msg_type;
    n2n_sock_t          sender_sn;


    traceDebug("process_sn_msg(%lu)", msg_size);

    sn_cpy(&sender_sn, (const n2n_sock_t *) sender_sock);
    sender_sn.port = htons(sender_sn.port);

    rem = msg_size; /* Counts down bytes of packet to protect against buffer overruns. */
    idx = 0; /* marches through packet header as parts are decoded. */
    if (decode_SNM_hdr(&hdr, msg_buf, &rem, &idx) < 0)
    {
        traceError("Failed to decode header");
        return -1; /* failed to decode packet */
    }
    log_SNM_hdr(&hdr);

    msg_type = hdr.type; /* message type */

    if (msg_type == SNM_TYPE_REQ_LIST_MSG)
    {
        n2n_SNM_REQ_t req;

        if (sss->snm_discovery_state != N2N_SNM_STATE_READY)
        {
            traceError("Received SNM REQ but supernode is NOT READY");
            return -1;
        }
        
        decode_SNM_REQ(&req, &hdr, msg_buf, &rem, &idx);
        log_SNM_REQ(&req);

        if (GET_A(hdr.flags))
        {
            /* request for ADV */

            if (GET_E(hdr.flags))
            {
                /* request from edge wanting to register a new community */
                struct comm_info *ci = NULL;
                int need_write = 0;

                if (req.comm_num != 1)
                {
                    traceError("Received SNM REQ from edge with comm_num=%d", req.comm_num);
                    return -1;
                }

                need_write = add_new_community(&sss->communities, req.comm_ptr[0].name, &ci);
                if (need_write)
                {
                    write_communities_to_file( sss->communities.filename,
                                              &sss->communities.head);

                    advertise_community_to_all(sss, ci->name);
                }
            }

            send_snm_adv(sss, &sender_sn, NULL);
        }
        else
        {
            /* request for INFO */
            send_snm_rsp(sss, &sender_sn, &hdr, &req);
        }

        if (!GET_E(hdr.flags))
        {
            update_and_save_supernodes(&sss->supernodes, &sender_sn, 1);
        }
    }
    else if (msg_type == SNM_TYPE_RSP_LIST_MSG)
    {
        n2n_SNM_INFO_t rsp;
        int sn_num = 0;
        struct sn_info *new_sn = NULL;

        if (sss->snm_discovery_state == N2N_SNM_STATE_READY)
        {
            traceError("Received SNM RSP but supernode is READY");
            return -1;
        }
        
        decode_SNM_INFO(&rsp, &hdr, msg_buf, &rem, &idx);
        log_SNM_INFO(&rsp);

        sn_num = process_snm_rsp(&sss->supernodes, &sss->communities,
                                 &sender_sn, &hdr, &rsp);

        /* send requests to the recently added supernodes */
        N2N_LIST_FOR_EACH_ENTRY(new_sn, &sss->supernodes.head)
        {
            if (sn_num == 0)
                break;

            send_snm_req(sss, &new_sn->sn, 1, NULL, 0);
            sn_num--;
        }
    }
    else if (msg_type == SNM_TYPE_ADV_MSG)
    {
        n2n_SNM_ADV_t adv;
        int communities_updated = 0;

        decode_SNM_ADV(&adv, &hdr, msg_buf, &rem, &idx);
        log_SNM_ADV(&adv);

        communities_updated = process_snm_adv(&sss->supernodes,
                                              &sss->communities,
                                              &sender_sn, &adv);

        if (communities_updated && GET_A(hdr.flags))
        {
            /* sending supernode is requesting ADV */
            send_snm_adv(sss, &sender_sn, &sss->communities.head);
        }

        /* new supernode will be advertised on REG SUPER ACK */
    }

    return 0;
}
#endif

/** Examine a datagram and determine what to do with it.
 *
 */
static int process_udp(n2n_sn_t *sss,
                       const struct sockaddr_in *sender_sock,
                       const uint8_t *udp_buf,
                       size_t udp_size,
                       time_t now)
{
    n2n_common_t        cmn; /* common fields in the packet header */
    size_t              rem;
    size_t              idx;
    size_t              msg_type;
    uint8_t             from_supernode;
    macstr_t            mac_buf;
    macstr_t            mac_buf2;
    n2n_sock_str_t      sockbuf;


    traceDebug("process_udp(%lu)", udp_size);

    /* Use decode_common() to determine the kind of packet then process it:
     *
     * REGISTER_SUPER adds an edge and generate a return REGISTER_SUPER_ACK
     *
     * REGISTER, REGISTER_ACK and PACKET messages are forwarded to their
     * destination edge. If the destination is not known then PACKETs are
     * broadcast.
     */

    rem = udp_size; /* Counts down bytes of packet to protect against buffer overruns. */
    idx = 0; /* marches through packet header as parts are decoded. */
    if (decode_common(&cmn, udp_buf, &rem, &idx) < 0)
    {
        traceError("Failed to decode common section");
        return -1; /* failed to decode packet */
    }

    msg_type = cmn.pc; /* packet code */
    from_supernode= cmn.flags & N2N_FLAGS_FROM_SUPERNODE;

    if (cmn.ttl < 1)
    {
        traceWarning("Expired TTL");
        return 0; /* Don't process further */
    }

    --(cmn.ttl); /* The value copied into all forwarded packets. */

    if (msg_type == MSG_TYPE_PACKET)
    {
        /* PACKET from one edge to another edge via supernode. */

        /* pkt will be modified in place and recoded to an output of potentially
         * different size due to addition of the socket.*/
        n2n_PACKET_t                    pkt; 
        n2n_common_t                    cmn2;
        uint8_t                         encbuf[N2N_SN_PKTBUF_SIZE];
        size_t                          encx=0;
        int                             unicast; /* non-zero if unicast */
        const uint8_t *                 rec_buf; /* either udp_buf or encbuf */


        sss->stats.last_fwd = now;
        decode_PACKET(&pkt, &cmn, udp_buf, &rem, &idx);

        unicast = (0 == is_multi_broadcast_mac(pkt.dstMac));

        traceDebug("Rx PACKET (%s) %s -> %s %s",
                   (unicast ? "unicast" : "multicast"),
                   macaddr_str(mac_buf, pkt.srcMac),
                   macaddr_str(mac_buf2, pkt.dstMac),
                   (from_supernode ? "from sn" : "local"));

        if (!from_supernode)
        {
            memcpy(&cmn2, &cmn, sizeof(n2n_common_t));

            /* We are going to add socket even if it was not there before */
            cmn2.flags |= N2N_FLAGS_SOCKET | N2N_FLAGS_FROM_SUPERNODE;

            pkt.sock.family = AF_INET;
            pkt.sock.port = ntohs(sender_sock->sin_port);
            memcpy(pkt.sock.addr.v4, &(sender_sock->sin_addr.s_addr), IPV4_SIZE);

            rec_buf = encbuf;

            /* Re-encode the header. */
            encode_PACKET(encbuf, &encx, &cmn2, &pkt);

            /* Copy the original payload unchanged */
            encode_buf(encbuf, &encx, (udp_buf + idx), (udp_size - idx));
        }
        else
        {
            /* Already from a supernode. Nothing to modify, just pass to
             * destination. */

            traceDebug("Rx PACKET fwd unmodified");

            rec_buf = udp_buf;
            encx = udp_size;
        }

        /* Common section to forward the final product. */
        if (unicast)
        {
            try_forward(sss, &cmn, pkt.dstMac, rec_buf, encx);
        }
        else
        {
            try_broadcast(sss, &cmn, pkt.srcMac, rec_buf, encx);
        }
    }/* MSG_TYPE_PACKET */
    else if (msg_type == MSG_TYPE_REGISTER)
    {
        /* Forwarding a REGISTER from one edge to the next */

        n2n_REGISTER_t                  reg;
        n2n_common_t                    cmn2;
        uint8_t                         encbuf[N2N_SN_PKTBUF_SIZE];
        size_t                          encx = 0;
        int                             unicast; /* non-zero if unicast */
        const uint8_t *                 rec_buf; /* either udp_buf or encbuf */

        sss->stats.last_fwd=now;
        decode_REGISTER( &reg, &cmn, udp_buf, &rem, &idx );

        unicast = (0 == is_multi_broadcast_mac(reg.dstMac));

        if (unicast)
        {
            traceDebug("Rx REGISTER %s -> %s %s",
                       macaddr_str(mac_buf, reg.srcMac),
                       macaddr_str(mac_buf2, reg.dstMac),
                       ((cmn.flags & N2N_FLAGS_FROM_SUPERNODE) ? "from sn" : "local"));

            if (0 != (cmn.flags & N2N_FLAGS_FROM_SUPERNODE))
            {
                memcpy(&cmn2, &cmn, sizeof(n2n_common_t));

                /* We are going to add socket even if it was not there before */
                cmn2.flags |= N2N_FLAGS_SOCKET | N2N_FLAGS_FROM_SUPERNODE;

                reg.sock.family = AF_INET;
                reg.sock.port = ntohs(sender_sock->sin_port);
                memcpy(reg.sock.addr.v4, &(sender_sock->sin_addr.s_addr), IPV4_SIZE);

                rec_buf = encbuf;

                /* Re-encode the header. */
                encode_REGISTER(encbuf, &encx, &cmn2, &reg);

                /* Copy the original payload unchanged */
                encode_buf(encbuf, &encx, (udp_buf + idx), (udp_size - idx));
            }
            else
            {
                /* Already from a supernode. Nothing to modify, just pass to
                 * destination. */

                rec_buf = udp_buf;
                encx = udp_size;
            }

            try_forward(sss, &cmn, reg.dstMac, rec_buf, encx); /* unicast only */
        }
        else
        {
            traceError("Rx REGISTER with multicast destination");
        }
    }
    else if (msg_type == MSG_TYPE_REGISTER_ACK)
    {
        traceDebug("Rx REGISTER_ACK (NOT IMPLEMENTED) SHould not be via supernode");
    }
    else if (msg_type == MSG_TYPE_REGISTER_SUPER)
    {
        n2n_REGISTER_SUPER_t            reg;
        n2n_REGISTER_SUPER_ACK_t        ack;
        n2n_common_t                    cmn2;
        uint8_t                         ackbuf[N2N_SN_PKTBUF_SIZE];
        size_t                          encx = 0;

        /* Edge requesting registration with us.  */
        
        sss->stats.last_reg_super = now;
        ++(sss->stats.reg_super);
        decode_REGISTER_SUPER(&reg, &cmn, udp_buf, &rem, &idx);

        init_cmn(&cmn2, n2n_register_super_ack,
                 N2N_FLAGS_SOCKET | N2N_FLAGS_FROM_SUPERNODE,
                 cmn.community);

        memcpy(&(ack.cookie), &(reg.cookie), sizeof(n2n_cookie_t));
        memcpy(ack.edgeMac, reg.edgeMac, sizeof(n2n_mac_t));
        ack.lifetime = reg_lifetime(sss);

        ack.sock.family = AF_INET;
        ack.sock.port = ntohs(sender_sock->sin_port);
        memcpy(ack.sock.addr.v4, &(sender_sock->sin_addr.s_addr), IPV4_SIZE);

        ack.num_sn = 0; /* No backup */
        memset(&(ack.sn_bak), 0, sizeof(n2n_sock_t));

        traceDebug("Rx REGISTER_SUPER for %s [%s]",
                   macaddr_str(mac_buf, reg.edgeMac),
                   sock_to_cstr(sockbuf, &(ack.sock)));

        update_edge(sss, reg.edgeMac, cmn.community, &(ack.sock), now);

#ifdef N2N_MULTIPLE_SUPERNODES
        {
            struct comm_info *ci = comm_find(&sss->communities.head,
                                             cmn.community, strlen((const char *) cmn.community));
            if (ci)
            {
                ack.num_sn = ci->sn_num;
                memcpy(&ack.sn_bak, ci->sn_sock, ci->sn_num * sizeof(n2n_sock_t));
            }
        }
#endif

        encode_REGISTER_SUPER_ACK(ackbuf, &encx, &cmn2, &ack);

        sendto(sss->sock, ackbuf, encx, 0,
               (struct sockaddr *) sender_sock, sizeof(struct sockaddr_in));

        traceDebug("Tx REGISTER_SUPER_ACK for %s [%s]",
                   macaddr_str(mac_buf, reg.edgeMac),
                   sock_to_cstr(sockbuf, &(ack.sock)));

    }


    return 0;
}


/** Help message to print if the command line arguments are not valid. */
static void exit_help(int argc, char * const argv[])
{
    fprintf(stderr, "%s usage\n", argv[0]);
    fprintf(stderr, "-l <lport>\tSet UDP main listen port to <lport>\n");

#ifdef N2N_MULTIPLE_SUPERNODES
    fprintf(stderr, "-s <snm_port>\tSet SNM listen port to <snm_port>\n");
    fprintf(stderr, "-i <ip:port>\tSet running SNM supernode to <ip:port>\n");
#endif

#if defined(N2N_HAVE_DAEMON)
    fprintf(stderr, "-f        \tRun in foreground.\n");
#endif /* #if defined(N2N_HAVE_DAEMON) */
    fprintf(stderr, "-v        \tIncrease verbosity. Can be used multiple times.\n");
    fprintf(stderr, "-h        \tThis help message.\n");
    fprintf(stderr, "\n");
    exit(1);
}

static int run_loop(n2n_sn_t *sss);

/* *********************************************** */

static const struct option long_options[] = {
  { "foreground",      no_argument,       NULL, 'f' },
  { "local-port",      required_argument, NULL, 'l' },
#ifdef N2N_MULTIPLE_SUPERNODES
  { "sn-port",         required_argument, NULL, 's' },
  { "supernode",       required_argument, NULL, 'i' },
#endif
  { "help"   ,         no_argument,       NULL, 'h' },
  { "verbose",         no_argument,       NULL, 'v' },
  { NULL,              0,                 NULL,  0  }
};

/** Main program entry point from kernel. */
int main(int argc, char * const argv[])
{
    n2n_sn_t sss;

    init_sn(&sss);

    {
        int opt;

#ifdef N2N_MULTIPLE_SUPERNODES
        const char *optstring = "fl:s:i:vh";
#else
        const char *optstring = "fl:vh";
#endif

        while ((opt = getopt_long(argc, argv, optstring, long_options, NULL)) != -1)
        {
            switch (opt)
            {
            case 'l': /* local-port */
                sss.lport = atoi(optarg);
                break;
#ifdef N2N_MULTIPLE_SUPERNODES
            case 's':
                sss.sn_port = atoi(optarg);
                break;
            case 'i':
            {
                n2n_sock_t sn;
                sock_from_cstr(&sn, optarg);
                update_supernodes(&sss.supernodes, &sn);
                break;
            }
#endif
            case 'f': /* foreground */
                sss.daemon = 0;
                break;
            case 'h': /* help */
                exit_help(argc, argv);
                break;
            case 'v': /* verbose */
                ++traceLevel;
                break;
            }
        }
    }

#if defined(N2N_HAVE_DAEMON)
    if (sss.daemon)
    {
        useSyslog = 1; /* traceEvent output now goes to syslog. */
        if (-1 == daemon(0, 0))
        {
            traceError("Failed to become daemon.");
            exit(-5);
        }
    }
#endif /* #if defined(N2N_HAVE_DAEMON) */

    traceDebug("traceLevel is %d", traceLevel);

    sss.sock = open_socket(sss.lport, 1 /*bind ANY*/);
    if (-1 == sss.sock)
    {
        traceError("Failed to open main socket. %s", strerror(errno));
        exit(-2);
    }
    else
    {
        traceNormal("supernode is listening on UDP %u (main)", sss.lport);
    }

    sss.mgmt_sock = open_socket(N2N_SN_MGMT_PORT, 0 /* bind LOOPBACK */);
    if (-1 == sss.mgmt_sock)
    {
        traceError("Failed to open management socket. %s", strerror(errno));
        exit(-2);
    }
    else
    {
        traceNormal("supernode is listening on UDP %u (management)", N2N_SN_MGMT_PORT);
    }

#ifdef N2N_MULTIPLE_SUPERNODES
    if (load_snm_info(&sss))
    {
        traceError("Failed to load SNM information. %s", strerror(errno));
        exit(-2);
    }

    sss.sn_sock = open_socket(sss.sn_port, 1 /* bind ANY */ );
    if (-1 == sss.sn_sock)
    {
        traceError("Failed to open supernodes communication socket. %s", strerror(errno));
        exit(-2);
    }

    traceNormal("supernode is listening on UDP %u (supernodes communication)", sss.sn_port);

    send_req_to_all_supernodes(&sss, (sss.snm_discovery_state != N2N_SNM_STATE_READY), NULL, 0);

#endif /* #ifdef N2N_MULTIPLE_SUPERNODES */

    traceNormal("supernode started");

    return run_loop(&sss);
}


/** Long lived processing entry point. Split out from main to simply
 *  daemonisation on some platforms. */
static int run_loop(n2n_sn_t *sss)
{
    uint8_t pktbuf[N2N_SN_PKTBUF_SIZE];
    int keep_running = 1;

    sss->start_time = time(NULL);

    while (keep_running)
    {
        int              rc;
        ssize_t          bread;
        int              max_sock;
        fd_set           socket_mask;
        struct timeval   wait_time;
        time_t           now = 0;

        FD_ZERO(&socket_mask);
        max_sock = MAX(sss->sock, sss->mgmt_sock);

#ifdef N2N_MULTIPLE_SUPERNODES
        max_sock = MAX(max_sock, sss->sn_sock);
        FD_SET(sss->sn_sock, &socket_mask);

        if (sss->snm_discovery_state != N2N_SNM_STATE_READY)
        {
            communities_discovery(sss, time(NULL));
        }
#endif

        FD_SET(sss->sock, &socket_mask);
        FD_SET(sss->mgmt_sock, &socket_mask);

        wait_time.tv_sec = 10;
        wait_time.tv_usec = 0;
        rc = select(max_sock + 1, &socket_mask, NULL, NULL, &wait_time);

        now = time(NULL);

        if (rc > 0)
        {
#ifdef N2N_MULTIPLE_SUPERNODES
            if (FD_ISSET(sss->sn_sock, &socket_mask))
            {
                struct sockaddr_in  sender_sock;
                size_t              i;

                i = sizeof(sender_sock);
                bread = recvfrom(sss->sn_sock, pktbuf, N2N_SN_PKTBUF_SIZE, 0/*flags*/,
                                 (struct sockaddr *)&sender_sock, (socklen_t*)&i);

                if (bread <= 0)
                {
                    traceError("recvfrom() failed %d errno %d (%s)", bread, errno, strerror(errno));
                    keep_running = 0;
                    break;
                }

                /* We have a datagram to process */
                process_sn_msg(sss, &sender_sock, pktbuf, bread, now);
            }
#endif

            if (FD_ISSET(sss->sock, &socket_mask)) 
            {
                struct sockaddr_in  sender_sock;
                socklen_t           i;

                i = sizeof(sender_sock);
                bread = recvfrom(sss->sock, pktbuf, N2N_SN_PKTBUF_SIZE, 0/*flags*/,
                                 (struct sockaddr *) &sender_sock, (socklen_t*) &i);

                if (bread < 0) /* For UDP bread of zero just means no data (unlike TCP). */
                {
                    /* The fd is no good now. Maybe we lost our interface. */
                    traceError("recvfrom() failed %d errno %d (%s)", bread, errno, strerror(errno));
                    keep_running = 0;
                    break;
                }

                /* We have a datagram to process */
                if (bread > 0)
                {
                    /* And the datagram has data (not just a header) */
                    process_udp(sss, &sender_sock, pktbuf, bread, now);
                }
            }

            if (FD_ISSET(sss->mgmt_sock, &socket_mask)) 
            {
                struct sockaddr_in  sender_sock;
                size_t              i;

                i = sizeof(sender_sock);
                bread = recvfrom(sss->mgmt_sock, pktbuf, N2N_SN_PKTBUF_SIZE, 0/*flags*/,
                                 (struct sockaddr *) &sender_sock, (socklen_t*) &i);

                if (bread <= 0)
                {
                    traceError("recvfrom() failed %d errno %d (%s)", bread, errno, strerror(errno));
                    keep_running = 0;
                    break;
                }

                /* We have a datagram to process */
                process_mgmt(sss, &sender_sock, pktbuf, bread, now);
            }
        }
        else
        {
            traceDebug("timeout");
        }

        purge_expired_registrations(&(sss->edges));

    } /* while */

    deinit_sn(sss);

    return 0;
}

