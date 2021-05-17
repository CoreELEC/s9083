
#include "common.h"
#include "wf_debug.h"

/* macro */
#if 0
#define MLME_DBG(fmt, ...)      LOG_D("[%s:%d]"fmt, __func__, __LINE__, ##__VA_ARGS__)
#define MLME_ARRAY(data, len)   log_array(data, len)
#else
#define MLME_DBG(fmt, ...)
#define MLME_ARRAY(data, len)
#endif
#define MLME_INFO(fmt, ...)     LOG_I("[%s:%d]"fmt, __func__, __LINE__, ##__VA_ARGS__)
#define MLME_WARN(fmt, ...)     LOG_W("[%s:%d]"fmt, __func__, __LINE__, ##__VA_ARGS__)
#define MLME_ERROR(fmt, ...)    LOG_E("[%s:%d]"fmt, __func__, __LINE__, ##__VA_ARGS__)

/* type define */
typedef struct
{
    wf_mlme_framework_e framework;
    scan_type_e type;
    wf_wlan_ssid_t ssids[WF_SCAN_REQ_SSID_NUM];
    wf_u8 ssid_num;
    wf_u8 chs[WF_SCAN_REQ_CHANNEL_NUM];
    wf_u8 ch_num;
} mlme_scan_t;
typedef wf_u8 mlme_scan_rsp_t[WF_80211_MGMT_PROBERSP_SIZE_MAX];

typedef struct
{
    wf_bool en_disconn_ind;
    wf_mlme_framework_e framework;
    wf_80211_bssid_t bssid;
    wf_wlan_ssid_t ssid;
} mlme_conn_t;

typedef struct
{
    wf_bool en_disconn_ind;
} mlme_deauth_t, mlme_conn_abort_t;

#ifdef CFG_ENABLE_ADHOC_MODE
typedef struct
{
    wf_mlme_framework_e framework;
    wf_wlan_ssid_t ssid;
    wf_u8 ch;
} mlme_conn_ibss_t;
#endif

/* function declaration */
static int mlme_msg_send (nic_info_st *pnic_info,
                          wf_msg_tag_t tag, void *value, wf_u8 len);

wf_inline static
int hw_cfg (nic_info_st *pnic_info, wdn_net_info_st *wdn_info)
{
    int ret = 0;
    wf_u16 basic_dr_cfg = 0;

    /* hardware configure */
    if (wdn_info->short_preamble)
    {
        ret |= wf_mcu_set_preamble(pnic_info, PREAMBLE_SHORT);
    }
    else
    {
        ret |= wf_mcu_set_preamble(pnic_info, PREAMBLE_LONG);
    }

    if (wdn_info->short_slot)
    {
        ret |= wf_mcu_set_slot_time(pnic_info, SHORT_SLOT_TIME);
    }
    else
    {
        ret |= wf_mcu_set_slot_time(pnic_info, NON_SHORT_SLOT_TIME);
    }

    if (wdn_info->wmm_enable)
    {
        ret |= wf_mcu_set_wmm_para_enable(pnic_info, wdn_info);
    }
    else
    {
        ret |= wf_mcu_set_wmm_para_disable(pnic_info, wdn_info);
    }

    ret |= wf_mcu_set_correct_tsf(pnic_info, wdn_info->tsf);

    ret |= wf_mcu_set_sifs(pnic_info, 0x0a0a0808);

    ret |= wf_mcu_set_macid_wakeup(pnic_info, wdn_info->wdn_id);

    ret |= wf_mcu_set_max_ampdu_len(pnic_info, wdn_info->htpriv.rx_ampdu_maxlen);
    ret |= wf_mcu_set_min_ampdu_space(pnic_info, wdn_info->htpriv.rx_ampdu_min_spacing);
    ret |= wf_mcu_set_config_xmit(pnic_info, WF_XMIT_AMPDU_DENSITY, wdn_info->htpriv.rx_ampdu_min_spacing);

    get_bratecfg_by_support_dates(wdn_info->datarate, wdn_info->datarate_len, &basic_dr_cfg);
    get_bratecfg_by_support_dates(wdn_info->ext_datarate, wdn_info->ext_datarate_len, &basic_dr_cfg);
    ret |= wf_mcu_set_basic_rate(pnic_info,  basic_dr_cfg);
    ret |= wf_mcu_msg_body_set_ability(pnic_info, ODM_FUNC_CLR, ODM_BB_DYNAMIC_TXPWR);

    return ret;
}

wf_inline static int build_wdn (nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wdn_net_info_st *pwdn_info;

    MLME_DBG();

    pmlme_info->pwdn_info = pwdn_info =
                                wf_wdn_add(pnic_info, wf_wlan_get_cur_bssid(pnic_info));
    if (pwdn_info == NULL)
    {
        MLME_WARN("new wdn fail");
        return -1;
    }

    if (wf_wdn_data_update(pnic_info, pwdn_info))
    {
        MLME_WARN("wdn update fail");
        return -2;
    }

    if (hw_cfg(pnic_info, pwdn_info))
    {
        MLME_WARN("hw config fail");
        return -3;
    }

    return 0;
}

static
wf_pt_rst_t core_scan_thrd (wf_pt_t *pt, nic_info_st *pnic_info,
                            mlme_scan_t *preq, int *prsn)
{
    wf_pt_t *pt_sub = &pt[1];
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_msg_que_t *pmsg_que = &pmlme_info->msg_que;
    wf_msg_t *pmsg;
    int rst;

    PT_BEGIN(pt);

    MLME_DBG();

    if (preq == NULL)
    {
        MLME_WARN("invalid scan request");
        *prsn = -1;
        PT_EXIT(pt);
    }

    /* start scan */
    rst = wf_scan_start(pnic_info, preq->type,
                        NULL,
                        preq->ssids, preq->ssid_num,
                        preq->chs, preq->ch_num);
    if (rst)
    {
        *prsn = -2;
        MLME_WARN("start fail, error code: %d", rst);
        PT_EXIT(pt);
    }

    /* scan process */
    MLME_INFO("scan...");
    PT_INIT(pt_sub);
    while (PT_SCHEDULE(wf_scan_thrd(pt_sub, pnic_info, prsn)))
    {
        if (!wf_msg_get(pmsg_que, &pmsg) && pmsg->tag == WF_MLME_TAG_SCAN_ABORT)
        {
            wf_msg_del(pmsg_que, pmsg);
            MLME_DBG("abort scanning...");
            wf_scan_stop(pnic_info);
            PT_WAIT_THREAD(pt, wf_scan_thrd(pt_sub, pnic_info, prsn));
            break;
        }
        PT_YIELD(pt);
    }


    /* notify system scan result */
    MLME_DBG("report scan result");
    wf_os_api_ind_scan_done(pnic_info, *prsn == WF_SCAN_TAG_ABORT,
                            preq->framework);
    *prsn = 0;

    PT_END(pt);
}

wf_inline static
int set_cur_network (nic_info_st *pnic_info,
                     wf_80211_mgmt_t *pmgmt, wf_u16 mgmt_len)
{
    wf_wlan_info_t *pwlan_info = pnic_info->wlan_info;
    wf_wlan_network_t *pcur_network = &pwlan_info->cur_network;
    wf_u16 var_len;
    wf_u8 bss_ch;

    /* get bss channel number */
    {
        wf_u8 *pies = &pmgmt->probe_resp.variable[0];
        wf_u16 ies_len = mgmt_len - WF_OFFSETOF(struct beacon_ie, variable);
        wf_80211_mgmt_ie_t *pie;
        if (wf_80211_mgmt_ies_search(pies, ies_len,
                                     WF_80211_MGMT_EID_DS_PARAMS, &pie))
        {
            MLME_WARN("no DS element field");
            return -1;
        }
        {
            wf_80211_mgmt_dsss_parameter_t *pds = (void *)pie->data;
            bss_ch = pds->current_channel;
        }
    }

    /* set channel */
    MLME_INFO("channel: %d", bss_ch);
    wf_wlan_set_cur_channel(pnic_info, bss_ch);
    /* retrive address */
    wf_memcpy(pcur_network->mac_addr, pmgmt->sa, sizeof(wf_80211_addr_t));
    wf_wlan_set_cur_bssid(pnic_info, pmgmt->bssid);
    /* retrive ssid */
    {
        wf_u8 *pies = &pmgmt->probe_resp.variable[0];
        wf_u16 ies_len = mgmt_len - WF_OFFSETOF(struct beacon_ie, variable);
        wf_80211_mgmt_ie_t *pie;
        int rst = wf_80211_mgmt_ies_search(pies, ies_len,
                                           WF_80211_MGMT_EID_SSID, &pie);
        if (rst)
        {
            MLME_ERROR("ies search fail, error code: %d", rst);
            return -2;
        }
        {
            wf_wlan_ssid_t ssid;
            ssid.length = pie->len;
            wf_memcpy(ssid.data, pie->data, pie->len);
            wf_wlan_set_cur_ssid(pnic_info, &ssid);
        }
    }
    /* retrive no elements field */
    pcur_network->timestamp = wf_le64_to_cpu(pmgmt->probe_resp.timestamp);
    pcur_network->bcn_interval = pmgmt->probe_resp.intv;
    pcur_network->cap_info = pmgmt->probe_resp.capab;
    /* copy ies */
    var_len = mgmt_len - WF_OFFSETOF(wf_80211_mgmt_t, probe_resp.variable);
    if (var_len > sizeof(pcur_network->ies))
    {
        MLME_WARN("mangnet frame body size beyond limit");
        return -2;
    }
    pcur_network->ies_length = var_len;
    wf_memcpy(&pcur_network->ies[0], &pmgmt->probe_resp.variable[0], var_len);

    return 0;
}

static
wf_pt_rst_t core_conn_scan_thrd (wf_pt_t *pt, nic_info_st *pnic_info,
                                 mlme_conn_t *preq, int *prsn)
{
    wf_pt_t *pt_sub = &pt[1];
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_msg_que_t *pmsg_que = &pmlme_info->msg_que;
    wf_msg_t *pmsg = NULL;
    int rst;

    PT_BEGIN(pt);

    if (preq == NULL)
    {
        MLME_WARN("invalid scan request");
        *prsn = -1;
        PT_EXIT(pt);
    }

    /* start scan */
    pmlme_info->try_cnt = 3;
retry :
    rst = wf_scan_start(pnic_info, SCAN_TYPE_ACTIVE,
                        preq->bssid,
                        &preq->ssid, preq->ssid.length ? 1 : 0,
                        NULL, 0);
    if (rst)
    {
        *prsn = -2;
        MLME_WARN("start fail, error code: %d", rst);
        PT_EXIT(pt);
    }
    MLME_INFO("wait probe respone...");

    /* scan process */
    PT_INIT(pt_sub);
    do
    {
        if (!PT_SCHEDULE(wf_scan_thrd(pt_sub, pnic_info, &rst)))
        {
            if (rst == WF_SCAN_TAG_DONE && --pmlme_info->try_cnt)
            {
                goto retry;
            }
            MLME_WARN("scan end, reason code: %d", rst);
            *prsn = -3;
            break;
        }

        if (!wf_msg_pop(pmsg_que, &pmsg))
        {
            if (pmsg->tag == WF_MLME_TAG_CONN_ABORT ||
                pmsg->tag == WF_MLME_TAG_SCAN_ABORT)
            {
                wf_msg_del(pmsg_que, pmsg);
                MLME_INFO("abort scanning...");
                wf_scan_stop(pnic_info);
                PT_WAIT_THREAD(pt, wf_scan_thrd(pt_sub, pnic_info, &rst));
                if (pmsg->tag == WF_MLME_TAG_CONN_ABORT)
                {
                    mlme_conn_abort_t *pconn_abort = (void *)pmsg->value;
                    mlme_conn_t *pconn = (void *)pmlme_info->pconn_msg->value;
                    pconn->en_disconn_ind = pconn_abort->en_disconn_ind;
                }
                *prsn = -4;
                PT_EXIT(pt);
            }
            if (pmsg->tag == WF_MLME_TAG_SCAN_RSP)
            {
                wf_80211_mgmt_t *pmgmt = (void *)pmsg->value;
                wf_u16 mgmt_len = pmsg->len;

                rst = set_cur_network(pnic_info, pmgmt, mgmt_len);
                if (rst)
                {
                    MLME_WARN("set cur_network fail, error code: %d", rst);
                }
                else
                {
                    wf_msg_del(pmsg_que, pmsg);
                    MLME_INFO("probe respone ok");
                    wf_scan_stop(pnic_info);
                    PT_WAIT_THREAD(pt, wf_scan_thrd(pt_sub, pnic_info, &rst));
                    *prsn = 0;
                    break;
                }
            }
            wf_msg_del(pmsg_que, pmsg);
        }

        PT_YIELD(pt);
    }
    while (wf_true);

    PT_END(pt);
}

static
wf_pt_rst_t core_conn_auth_thrd (wf_pt_t *pt, nic_info_st *pnic_info, int *prsn)
{
    wf_pt_t *pt_sub = &pt[1];
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_msg_que_t *pmsg_que = &pmlme_info->msg_que;
    wf_msg_t *pmsg = NULL;
    int rst;

    PT_BEGIN(pt);

    rst = wf_auth_sta_start(pnic_info);
    if (rst)
    {
        MLME_WARN("start fail, error code: %d", rst);
        *prsn = -1;
        PT_EXIT(pt);
    }

    PT_INIT(pt_sub);
    while (PT_SCHEDULE(wf_auth_sta_thrd(pt_sub, pnic_info, prsn)))
    {
        if (!wf_msg_pop(pmsg_que, &pmsg))
        {
            if (pmsg->tag == WF_MLME_TAG_CONN_ABORT ||
                pmsg->tag == WF_MLME_TAG_DEAUTH)
            {
                wf_msg_del(pmsg_que, pmsg);
                MLME_INFO("abort auth...");
                wf_auth_sta_stop(pnic_info);
                PT_WAIT_THREAD(pt, wf_auth_sta_thrd(pt_sub, pnic_info, prsn));
                if (pmsg->tag == WF_MLME_TAG_CONN_ABORT)
                {
                    mlme_conn_abort_t *pconn_abort = (void *)pmsg->value;
                    mlme_conn_t *pconn = (void *)pmlme_info->pconn_msg->value;
                    pconn->en_disconn_ind = pconn_abort->en_disconn_ind;
                }
                break;
            }
            wf_msg_del(pmsg_que, pmsg);
        }
        PT_YIELD(pt);
    }
    if (*prsn != WF_AUTH_TAG_DONE)
    {
        PT_EXIT(pt);
    }

    PT_END(pt);
}

static
wf_pt_rst_t core_conn_assoc_thrd (wf_pt_t *pt, nic_info_st *pnic_info, int *prsn)
{
    wf_pt_t *pt_sub = &pt[1];
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_msg_que_t *pmsg_que = &pmlme_info->msg_que;
    wf_msg_t *pmsg = NULL;
    int rst;

    PT_BEGIN(pt);

    rst = wf_assoc_start(pnic_info);
    if (rst)
    {
        MLME_WARN("start fail, error code: %d", rst);
        *prsn = -1;
        PT_EXIT(pt);
    }

    PT_INIT(pt_sub);
    while (PT_SCHEDULE(wf_assoc_sta_thrd(pt_sub, pnic_info, prsn)))
    {
        if (!wf_msg_pop(pmsg_que, &pmsg))
        {
            if (pmsg->tag == WF_MLME_TAG_CONN_ABORT ||
                pmsg->tag == WF_MLME_TAG_DEAUTH)
            {
                wf_msg_del(pmsg_que, pmsg);
                MLME_INFO("abort assoc...");
                wf_assoc_stop(pnic_info);
                PT_WAIT_THREAD(pt, wf_assoc_sta_thrd(pt_sub, pnic_info, prsn));
                if (pmsg->tag == WF_MLME_TAG_CONN_ABORT)
                {
                    mlme_conn_abort_t *pconn_abort = (void *)pmsg->value;
                    mlme_conn_t *pconn = (void *)pmlme_info->pconn_msg->value;
                    pconn->en_disconn_ind = pconn_abort->en_disconn_ind;
                }
                break;
            }
            wf_msg_del(pmsg_que, pmsg);
        }
        PT_YIELD(pt);
    }
    if (*prsn != WF_ASSOC_TAG_DONE)
    {
        wf_deauth_xmit_frame(pnic_info, wf_wlan_get_cur_bssid(pnic_info),
                             WF_80211_REASON_DEAUTH_LEAVING);
        PT_EXIT(pt);
    }

    PT_END(pt);
}

int core_conn_preconnect (nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wdn_net_info_st *pwdn_info = pmlme_info->pwdn_info;
    hw_info_st *phw_info = pnic_info->hw_info;

    if (phw_info->ba_enable == wf_true)
    {
        rx_info_t *rx_info = pnic_info->rx_info;
        if(NULL != rx_info)
        {
            pwdn_info->ba_ctl = rx_info->ba_ctl;
        }
    }

#if defined(CONFIG_CONCURRENT_MODE) && defined(CFG_ENABLE_AP_MODE)
    if(get_sys_work_mode(pnic_info->vir_nic) == WF_MASTER_MODE)
    {
        wf_u8 channel;
        wf_u8 bw_mode;
        wf_u8 offset;
        channel = pwdn_info->channel;
        bw_mode = pwdn_info->bw_mode;
        offset = pwdn_info->channle_offset;
        /* concurrent mode ap reset channel*/
        LOG_I("sta nic_num:%d",pnic_info->nic_num);
        LOG_I("channel:%d bw_mode:%d offset:%d",channel,bw_mode,offset);
        if(pnic_info->nic_num == 0)
        {
            wf_hw_info_set_channnel_bw(pnic_info->vir_nic, channel, bw_mode, offset);
            wf_resend_bcn(pnic_info->vir_nic, channel);
        }
        else if(pnic_info->nic_num == 1)
        {
            wf_hw_info_set_channnel_bw(pnic_info->vir_nic, channel, bw_mode, offset);
            wf_resend_bcn(pnic_info->vir_nic, channel);
        }
    }
#endif

    if (wf_hw_info_set_channnel_bw(pnic_info,
                                   pwdn_info->channel,
                                   pwdn_info->bw_mode,
                                   pwdn_info->channle_offset))
    {
        MLME_WARN("UMSG_OPS_HAL_CHNLBW_MODE failed");
        return -1;
    }
    if (wf_mcu_wdn_update(pnic_info, pwdn_info))
    {
        MLME_WARN("ODM Update Failed");
        return -2;
    }

    wf_mcu_set_user_info(pnic_info, wf_true);
    wf_action_frame_del_ba_request(pnic_info);
    wf_mcu_set_mlme_join(pnic_info, 2);
    wf_os_api_enable_all_data_queue(pnic_info->ndev);
    {
        mlme_conn_t *pconn_req = (void *)pmlme_info->pconn_msg->value;
        wf_os_api_ind_connect(pnic_info, pconn_req->framework);
    }

#ifdef CONFIG_ARS_SUPPORT
    {
        ars_st *pars = pnic_info->ars;
        wf_ars_info_update(pnic_info);
        //ars_thread_sema_post(pars);
        wf_os_api_timer_set(&pars->ars_thread.thread_timer,2000);
    }
#endif

    return 0;
}

static
wf_pt_rst_t core_conn_maintain_scan_thrd (wf_pt_t *pt, nic_info_st *pnic_info,
        mlme_scan_t *preq, int *prsn)
{
    wf_pt_t *pt_sub = &pt[1];
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_msg_que_t *pmsg_que = &pmlme_info->msg_que;
    wf_msg_t *pmsg;
    int rst;

    PT_BEGIN(pt);

    rst = wf_scan_start(pnic_info, SCAN_TYPE_PASSIVE,
                        NULL,
                        NULL, 0,
                        NULL, 0);
    if (rst)
    {
        MLME_WARN("start fail error code: %d", *prsn);
        *prsn = -1;
        PT_EXIT(pt);
    }

    PT_INIT(pt_sub);
    while (PT_SCHEDULE(wf_scan_thrd(pt_sub, pnic_info, prsn)))
    {
        if (!wf_msg_get(pmsg_que, &pmsg))
        {
            if (pmsg->tag == WF_MLME_TAG_SCAN_ABORT)
            {
                MLME_DBG("abort scanning...");
                wf_scan_stop(pnic_info);
                wf_msg_del(pmsg_que, pmsg);
                PT_WAIT_THREAD(pt, wf_scan_thrd(pt_sub, pnic_info, prsn));
                break;
            }
            else if (pmsg->tag == WF_MLME_TAG_CONN_ABORT ||
                     pmsg->tag == WF_MLME_TAG_DEAUTH ||
                     pmsg->tag == WF_MLME_TAG_DEASSOC)
            {
                wf_scan_stop(pnic_info);
                PT_WAIT_THREAD(pt, wf_scan_thrd(pt_sub, pnic_info, prsn));
                break;
            }
        }
        PT_YIELD(pt);
    }
    /* notify system scan result */
    wf_os_api_ind_scan_done(pnic_info, *prsn == WF_SCAN_TAG_ABORT,
                            preq->framework);

    PT_END(pt);
}

static
wf_pt_rst_t core_conn_maintain_probe_thrd (wf_pt_t *pt, nic_info_st *pnic_info,
        int *prsn)
{
    wf_pt_t *pt_sub = &pt[1];
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_msg_que_t *pmsg_que = &pmlme_info->msg_que;
    wf_msg_t *pmsg;
    int rst;

    PT_BEGIN(pt);

    {
        wf_wlan_ssid_t ssid;
        wdn_net_info_st *pwdn_info = pmlme_info->pwdn_info;
        if (pwdn_info == NULL)
        {
            MLME_ERROR("wdn null");
            *prsn = -1;
            PT_EXIT(pt);
        }
        wf_memcpy(&ssid.data, pwdn_info->ssid,
                  WF_MIN(sizeof(wf_80211_mgmt_ssid_t), WF_SSID_LEN));
        rst = wf_scan_start(pnic_info, SCAN_TYPE_ACTIVE,
                            (void *)pwdn_info->bssid,
                            &ssid, 1,
                            &pwdn_info->channel, 1);
    }
    if (rst)
    {
        MLME_WARN("start fail error code: %d", rst);
        *prsn = -2;
        PT_EXIT(pt);
    }

    PT_INIT(pt_sub);
    while (PT_SCHEDULE(wf_scan_thrd(pt_sub, pnic_info, prsn)))
    {
        if (!wf_msg_get(pmsg_que, &pmsg))
        {
            if (pmsg->tag == WF_MLME_TAG_SCAN_ABORT ||
                pmsg->tag == WF_MLME_TAG_SCAN_RSP)
            {
                wf_msg_del(pmsg_que, pmsg);
                MLME_DBG("abort scanning...");
                wf_scan_stop(pnic_info);
                PT_WAIT_THREAD(pt, wf_scan_thrd(pt_sub, pnic_info, prsn));
                break;
            }
            else if (pmsg->tag == WF_MLME_TAG_CONN_ABORT ||
                     pmsg->tag == WF_MLME_TAG_DEAUTH ||
                     pmsg->tag == WF_MLME_TAG_DEASSOC)
            {
                wf_scan_stop(pnic_info);
                PT_WAIT_THREAD(pt, wf_scan_thrd(pt_sub, pnic_info, prsn));
                break;
            }
        }
        PT_YIELD(pt);
    }

    PT_END(pt);
}

wf_inline int core_conn_maintain_deauth (nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    hw_info_st *phw_info = pnic_info->hw_info;

    wf_mcu_set_user_info(pnic_info, wf_false);
    wf_action_frame_del_ba_request(pnic_info);
    wf_deauth_xmit_frame(pnic_info, wf_wlan_get_cur_bssid(pnic_info),
                         WF_80211_REASON_DEAUTH_LEAVING);
    if (phw_info->ba_enable == wf_true)
    {
        if (NULL != pmlme_info->pwdn_info->ba_ctl)
        {
            pmlme_info->pwdn_info->ba_ctl = NULL;
        }
    }
    wf_mcu_set_user_info(pnic_info, wf_true);

    return 0;
}

wf_inline int core_conn_maintain_deassoc (nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    hw_info_st *phw_info = pnic_info->hw_info;

    wf_mcu_set_user_info(pnic_info, wf_false);
    wf_action_frame_del_ba_request(pnic_info);

    if (phw_info->ba_enable == wf_true)
    {
        if (NULL != pmlme_info->pwdn_info->ba_ctl)
        {
            pmlme_info->pwdn_info->ba_ctl = NULL;
        }
    }

    wf_mcu_set_user_info(pnic_info, wf_true);

    return 0;
}

wf_inline int core_conn_maintain_ba_req (nic_info_st *pnic_info)
{
    hw_info_st *phw_info = pnic_info->hw_info;
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wdn_net_info_st *pwdn_info = pmlme_info->pwdn_info;
    if (pwdn_info == NULL)
    {
        return -1;
    }

    if (phw_info->ba_enable)
    {
        if (wf_action_frame_ba_to_issue(pnic_info,
                                        WF_WLAN_ACTION_ADDBA_REQ) < 0)
        {
            MLME_WARN("*** wf_action_frame_ba_to_issue(WF_WLAN_ACTION_ADDBA_REQ) failed***");
        }
        pwdn_info->ba_started_flag[pmlme_info->bareq_parm.tid] = wf_true;
    }

    return 0;
}

wf_inline int core_conn_maintain_ba_rsp (nic_info_st *pnic_info)
{
    hw_info_st *phw_info = pnic_info->hw_info;
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wdn_net_info_st *pwdn_info = pmlme_info->pwdn_info;

    if (pwdn_info == NULL)
    {
        return -1;
    }

    if (phw_info->ba_enable)
    {
        if (wf_action_frame_ba_to_issue(pnic_info,
                                        WF_WLAN_ACTION_ADDBA_RESP) < 0)
        {
            pwdn_info->ba_ctl[pmlme_info->barsp_parm.tid].enable = wf_false;
            MLME_WARN("*** wf_action_frame_ba_to_issue(WF_WLAN_ACTION_ADDBA_RESP) failed***");
        }
        else
        {
            pwdn_info->ba_ctl[pmlme_info->barsp_parm.tid].enable = wf_true;
            pwdn_info->ba_ctl[pmlme_info->barsp_parm.tid].wait_timeout =
                pmlme_info->bareq_parm.timeout;
        }
    }

    return 0;
}

static
wf_pt_rst_t core_conn_maintain_msg_thrd (wf_pt_t *pt, nic_info_st *pnic_info)
{
    wf_pt_t *pt_sub = &pt[1];
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_msg_que_t *pmsg_que = &pmlme_info->msg_que;
    wf_msg_t *pmsg;
    int reason;
#ifdef CONFIG_LPS
    mlme_lps_t *param;
#endif

    PT_BEGIN(pt);

    for (;;)
    {
        mlme_set_state(pnic_info, MLME_STATE_IDLE);
        /* wait new message */
        PT_YIELD_UNTIL(pt, !wf_msg_pop(pmsg_que, &pmsg));

        if (pmsg->tag == WF_MLME_TAG_SCAN)
        {
            pmlme_info->pscan_msg = pmsg;
            MLME_INFO("scan...");
            mlme_set_state(pnic_info, MLME_STATE_SCAN);
            PT_SPAWN(pt, pt_sub,
                     core_conn_maintain_scan_thrd(pt_sub, pnic_info,
                                                  (mlme_scan_t *)pmlme_info->pscan_msg->value,
                                                  &reason));
            wf_msg_del(pmsg_que, pmlme_info->pscan_msg);
            pmlme_info->pscan_msg = NULL;
        }

        else if (pmsg->tag == WF_MLME_TAG_KEEPALIVE)
        {
            wf_msg_del(pmsg_que, pmsg);
            MLME_INFO("keepalive...");
            mlme_set_state(pnic_info, MLME_STATE_SCAN);
            PT_SPAWN(pt, pt_sub,
                     core_conn_maintain_probe_thrd(pt_sub, pnic_info, &reason));
        }

        else if (pmsg->tag == WF_MLME_TAG_DEAUTH ||
                 pmsg->tag == WF_MLME_TAG_CONN_ABORT)
        {
            wf_msg_del(pmsg_que, pmsg);
            MLME_INFO("deauth");
            mlme_set_state(pnic_info, MLME_STATE_DEAUTH);
            core_conn_maintain_deauth(pnic_info);
            {
                mlme_conn_abort_t *pconn_abort = (void *)pmsg->value;
                mlme_conn_t *pconn = (void *)pmlme_info->pconn_msg->value;
                pconn->en_disconn_ind = pconn_abort->en_disconn_ind;
            }
            break;
        }

        else if (pmsg->tag == WF_MLME_TAG_DEASSOC)
        {
            wf_msg_del(pmsg_que, pmsg);
            MLME_INFO("deassoc");
            mlme_set_state(pnic_info, MLME_STATE_DEASSOC);
            core_conn_maintain_deassoc(pnic_info);
            break;
        }

        else if (pmsg->tag == WF_MLME_TAG_ADD_BA_REQ)
        {
            wf_msg_del(pmsg_que, pmsg);
            MLME_INFO("ba request");
            mlme_set_state(pnic_info, MLME_STATE_ADD_BA_REQ);
            core_conn_maintain_ba_req(pnic_info);
        }

        else if (pmsg->tag == WF_MLME_TAG_ADD_BA_RSP)
        {
            wf_msg_del(pmsg_que, pmsg);
            MLME_INFO("ba respone");
            mlme_set_state(pnic_info, MLME_STATE_ADD_BA_RESP);
            core_conn_maintain_ba_rsp(pnic_info);
        }

#ifdef CONFIG_LPS
        else if (pmsg->tag == WF_MLME_TAG_LPS)
        {
            param = (mlme_lps_t *) pmsg->value;
            MLME_INFO("msg.module: %s", "MLME_MSG_LPS");

            wf_lps_ctrl_state_hdl(pnic_info, param->lps_ctrl_type);

            wf_msg_del(pmsg_que, pmsg);
        }
#endif

        else
        {
            wf_msg_del(pmsg_que, pmsg);
            MLME_INFO("unsuited message(tag: %d)", pmsg->tag);
        }
    }

    PT_END(pt);
}

static int core_conn_maintain_traffic (nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info  = (mlme_info_t*)pnic_info->mlme_info;
    tx_info_st *ptx_info = pnic_info->tx_info;
    wf_u16 BusyThreshold;

#if 0
    MLME_DBG("num_tx_ok_in_period=%d  num_rx_ok_in_period=%d",
             pmlme_info->link_info.num_tx_ok_in_period,
             pmlme_info->link_info.num_rx_ok_in_period);
#endif

    if (ptx_info == NULL)
    {
        MLME_WARN("tx_info NULL");
    }

    if (!wf_timer_expired(&pmlme_info->traffic_timer))
    {
        return 0;
    }
    wf_timer_reset(&pmlme_info->traffic_timer);

    {
        wf_u16 BusyThresholdHigh    = 100;
        wf_u16 BusyThresholdLow     = 75;
        BusyThreshold = pmlme_info->link_info.busy_traffic ?
                        BusyThresholdLow : BusyThresholdHigh;
        if (pmlme_info->link_info.num_rx_ok_in_period > BusyThreshold ||
            pmlme_info->link_info.num_tx_ok_in_period > BusyThreshold)
        {
            pmlme_info->link_info.busy_traffic = wf_true;
        }
        else
        {
            pmlme_info->link_info.busy_traffic = wf_false;
        }
    }

    {
        int i;
        for (i = 0; i < TID_NUM; i++)
        {
            pmlme_info->link_info.num_tx_ok_in_period_with_tid[i] = 0;
        }
        pmlme_info->link_info.num_rx_ok_in_period = 0;
        pmlme_info->link_info.num_tx_ok_in_period = 0;
        pmlme_info->link_info.num_rx_unicast_ok_in_period = 0;
    }

    return 0;
}

static wf_pt_rst_t
core_conn_maintain_keepalive_thrd (wf_pt_t *pt,  nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wdn_net_info_st *pwdn_info = pmlme_info->pwdn_info;
    int rst;

    PT_BEGIN(pt);

    for (;;)
    {
        PT_WAIT_WHILE(pt, pmlme_info->link_info.busy_traffic);

        pwdn_info->rx_pkt_stat_last = pwdn_info->rx_pkt_stat;
        wf_timer_set(&pmlme_info->keep_alive_timer, 10 * 1000);
        PT_WAIT_UNTIL(pt, wf_timer_expired(&pmlme_info->keep_alive_timer));
        if (pwdn_info->rx_pkt_stat_last != pwdn_info->rx_pkt_stat)
        {
            continue;
        }
        rst = mlme_msg_send(pnic_info, WF_MLME_TAG_KEEPALIVE, NULL, 0);
        if (rst)
        {
            MLME_WARN("mlme_msg_send fail, error code: %d", rst);
            continue;
        }

        wf_timer_set(&pmlme_info->keep_alive_timer, 10 * 1000);
        PT_WAIT_UNTIL(pt, wf_timer_expired(&pmlme_info->keep_alive_timer));
        if (pwdn_info->rx_pkt_stat_last != pwdn_info->rx_pkt_stat)
        {
            continue;
        }
        rst = wf_mlme_deauth(pnic_info, wf_true);
        if (rst)
        {
            MLME_WARN("wf_mlme_deauth fail, error code: %d", rst);
            continue;
        }

        break;
    }

    PT_END(pt);
}

wf_inline static
int core_conn_maintain (wf_pt_t *pt, nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_pt_t *pt_keepalive = &pt[1];
    wf_pt_t *pt_msg = &pt[2];
#ifdef CONFIG_LPS
    wf_pt_t *pt_lps = &pt[5];
#endif

    PT_BEGIN(pt);

    PT_INIT(pt_keepalive);
    PT_INIT(pt_msg);
    wf_timer_set(&pmlme_info->traffic_timer, 1000);
    do
    {
        core_conn_maintain_traffic(pnic_info);
#ifdef CONFIG_LPS
        wf_lps_sleep_mlme_monitor(pt_lps, pnic_info);
#endif
        core_conn_maintain_keepalive_thrd(pt_keepalive, pnic_info);

        PT_YIELD(pt);
    }
    while (PT_SCHEDULE(core_conn_maintain_msg_thrd(pt_msg, pnic_info)));

    PT_END(pt);
}

wf_inline static int mlme_conn_clearup (nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_wlan_info_t *pwlan_info = (wf_wlan_info_t *)pnic_info->wlan_info;

    MLME_INFO();

    pmlme_info->link_info.busy_traffic = wf_false;
    if (pmlme_info->pwdn_info)
    {
        wf_wdn_remove(pnic_info, pwlan_info->cur_network.bssid);
        pmlme_info->pwdn_info = NULL;
    }
    wf_memset(&pwlan_info->cur_network.mac_addr, 0x0, sizeof(wf_80211_addr_t));
    wf_memset(&pwlan_info->cur_network.bssid, 0x0, sizeof(wf_80211_addr_t));

    return 0;
}

#ifdef CFG_ENABLE_ADHOC_MODE
wf_pt_rst_t
adhoc_conn_maintain_msg (wf_pt_t *pt, nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_msg_que_t *pmsg_que = &pmlme_info->msg_que;
    wf_adhoc_info_t *adhoc_info = pnic_info->adhoc_info;
    wf_msg_t *pmsg;
    wf_80211_mgmt_t *pmgmt;
    wdn_net_info_st *pwdn_info;

    PT_BEGIN(pt);

    MLME_DBG();

    for (;;)
    {
        PT_YIELD_UNTIL(pt, !wf_msg_pop(pmsg_que, &pmsg));

        if (pmsg->tag == WF_MLME_TAG_IBSS_LEAVE)
        {
            MLME_INFO("mlme  leave ibss...");
            wf_msg_del(pmsg_que, pmsg);
            wf_mlme_abort(pnic_info);
            PT_EXIT(pt);
        }
        else if (pmsg->tag == WF_MLME_TAG_IBSS_BEACON_FRAME)
        {
            /* create wdn if no find the node */
            pmgmt=(wf_80211_mgmt_t *)pmsg->value;

            pwdn_info = wf_wdn_find_info(pnic_info, pmgmt->sa);
            if (pwdn_info)
            {
                MLME_WARN("bss has been build");
                wf_msg_del(pmsg_que, pmsg);
                return 0;
            }

            pwdn_info = wf_wdn_add(pnic_info, pmgmt->sa);
            if (!pwdn_info)
            {
                MLME_WARN("wdn add fail");
            }
            else
            {
                adhoc_info->asoc_sta_count++;
                wf_adhoc_wdn_info_update(pnic_info, pwdn_info);
                if (wf_adhoc_prc_bcn(pnic_info, pmsg, pwdn_info))
                {
                    wf_wdn_remove(pnic_info, pmgmt->sa);
                }

                if(adhoc_info->asoc_sta_count == 2)
                {
                    wf_os_api_ind_connect(pnic_info, adhoc_info->framework);
                }
            }
        }

        wf_msg_del(pmsg_que, pmsg);
    }

    PT_END(pt);
}

static
wf_pt_rst_t core_conn_scan_ibss_thrd (wf_pt_t *pt, nic_info_st *pnic_info,
                                      mlme_conn_ibss_t *preq, int *prsn)
{
    wf_pt_t *pt_sub = &pt[1];
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_msg_que_t *pmsg_que = &pmlme_info->msg_que;
    wf_msg_t *pmsg;
    int rst;

    PT_BEGIN(pt);

    if (preq == NULL)
    {
        MLME_WARN("invalid scan request");
        *prsn = -1;
        PT_EXIT(pt);
    }

    /* start scan */
    rst = wf_scan_start(pnic_info, SCAN_TYPE_ACTIVE,
                        NULL,
                        &preq->ssid, 1,
                        &preq->ch, 1);
    if (rst)
    {
        *prsn = -2;
        MLME_WARN("start fail, error code: %d", rst);
        PT_EXIT(pt);
    }
    MLME_INFO("wait probe respone...");


    /* scan process */
    PT_INIT(pt_sub);
    do
    {
        if (!PT_SCHEDULE(wf_scan_thrd(pt_sub, pnic_info, &rst)))
        {
            MLME_WARN("scan end, reason code: %d", rst);
            *prsn = -3;
            PT_EXIT(pt);
        }

        if(pnic_info->is_surprise_removed)
        {
            *prsn = -1;
            PT_EXIT(pt);
        }

        if (!wf_msg_pop(pmsg_que, &pmsg))
        {
            if (pmsg->tag == WF_MLME_TAG_SCAN_RSP)
            {
                wf_80211_mgmt_t *pmgmt = (void *)pmsg->value;
                wf_u16 mgmt_len = pmsg->len;
                wf_wlan_operation_mode_e opr_mode;
                wf_bool privacy;

                privacy = !!(pmgmt->probe_resp.capab & WF_80211_MGMT_CAPAB_PRIVACY);
                opr_mode = (pmgmt->probe_resp.capab & WF_80211_MGMT_CAPAB_ESS) ?
                           WF_WLAN_OPR_MODE_MASTER : WF_WLAN_OPR_MODE_ADHOC;
                if (opr_mode == WF_WLAN_OPR_MODE_ADHOC && privacy == wf_false)
                {
                    rst = set_cur_network(pnic_info, pmgmt, mgmt_len);
                    if (rst)
                    {
                        MLME_WARN("set cur_network fail, error code: %d", rst);
                    }
                    else
                    {
                        wf_msg_del(pmsg_que, pmsg);
                        wf_scan_stop(pnic_info);
                        PT_WAIT_THREAD(pt, wf_scan_thrd(pt_sub, pnic_info, &rst));
                        *prsn = 0;
                        break;
                    }
                }
            }
            wf_msg_del(pmsg_que, pmsg);
        }

        PT_YIELD(pt);
    }
    while (wf_true);

    PT_END(pt);
}
#endif

static wf_pt_rst_t mlme_core_thrd (nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    wf_pt_t *pt = &pmlme_info->pt[0], *pt_sub = &pt[1];
#ifdef CFG_ENABLE_ADHOC_MODE
    wf_adhoc_info_t *adhoc_info = pnic_info->adhoc_info;
    mlme_conn_ibss_t *param;
#endif
    wf_msg_que_t *pmsg_que = &pmlme_info->msg_que;
    wf_msg_t *pmsg;
    int reason;

    PT_BEGIN(pt);

    while (wf_true)
    {
        if (pmlme_info->babort_thrd)
        {
            MLME_INFO("thread abort");
            PT_EXIT(pt);
        }
        if (!wf_msg_pop(pmsg_que, &pmsg))
        {
            break;
        }
        PT_YIELD(pt);
    }

    if (pmsg->tag == WF_MLME_TAG_SCAN)
    {
        mlme_set_state(pnic_info, MLME_STATE_SCAN);
        /* retrive message information */
        pmlme_info->pscan_msg = pmsg;
        /* do scan process */
        MLME_INFO("scanning...");
        PT_SPAWN(pt, pt_sub, core_scan_thrd(pt_sub, pnic_info,
                                            (void *)pmlme_info->pscan_msg->value,
                                            &reason));
        /* delete scan request message */
        wf_msg_del(pmsg_que, pmlme_info->pscan_msg);
        pmlme_info->pscan_msg = NULL;
    }

    else if (pmsg->tag == WF_MLME_TAG_CONN)
    {
        MLME_INFO("start conneting to bss: \"%s\"",
                  ((mlme_conn_t *)pmsg->value)->ssid.data);

        mlme_set_state(pnic_info, MLME_STATE_CONN_SCAN);
        /* retrive message information */
        pmlme_info->pconn_msg = pmsg;
        /* launch probe request to find target bss */
        MLME_INFO("search bss...");
        PT_SPAWN(pt, pt_sub,
                 core_conn_scan_thrd(pt_sub, pnic_info,
                                     (void *)pmlme_info->pconn_msg->value,
                                     &reason));
        if (reason)
        {
            MLME_WARN("search bss fail, error code: %d", reason);
            goto conn_break;
        }
        MLME_INFO("found bss");

        /* make a new wdn */
        MLME_INFO("build wdn infomation");
        reason = build_wdn(pnic_info);
        if (reason)
        {
            MLME_WARN("new wdn fail, error code: %d", reason);
            goto conn_break;
        }

        /* auth process */
        MLME_INFO("auth...");
        mlme_set_state(pnic_info, MLME_STATE_AUTH);
        PT_SPAWN(pt, pt_sub, core_conn_auth_thrd(pt_sub, pnic_info, &reason));
        if (reason != WF_AUTH_TAG_DONE)
        {
            MLME_WARN("auth fail: auth error code: %d", reason);
            goto conn_break;
        }
        MLME_INFO("auth success");

        /* assoc process */
        MLME_INFO("assoc...");
        mlme_set_state(pnic_info, MLME_STATE_ASSOC);
        PT_SPAWN(pt, pt_sub, core_conn_assoc_thrd(pt_sub, pnic_info, &reason));
        if (reason != WF_ASSOC_TAG_DONE)
        {
            MLME_WARN("assoc fail: assoc error code: %d", reason);
            goto conn_break;
        }
        MLME_INFO("assoc success");

        /* prepare connect handle */
        reason = core_conn_preconnect(pnic_info);
        if (reason)
        {
            MLME_WARN("connect fail: preconnect error code: %d", reason);
            goto conn_break;
        }
        wf_rx_ba_all_reinit(pnic_info);
        wf_mlme_set_connect(pnic_info, wf_true);

        MLME_INFO("connect success");

        /* connection maintain handler */
        MLME_INFO("connection maintain");
        PT_SPAWN(pt, pt_sub, core_conn_maintain(pt_sub, pnic_info));
        wf_mlme_set_connect(pnic_info, wf_false);
        MLME_INFO("connection break");

    conn_break:
        {
            mlme_conn_t *pconn_req =
                (mlme_conn_t *)pmlme_info->pconn_msg->value;
            if (pconn_req->en_disconn_ind)
            {
                wf_os_api_ind_disconnect(pnic_info, pconn_req->framework);
            }
        }
        mlme_conn_clearup(pnic_info);
        wf_msg_del(pmsg_que, pmlme_info->pconn_msg);
        pmlme_info->pconn_msg = NULL;
    }

#ifdef CFG_ENABLE_ADHOC_MODE
    else if(pmsg->tag == WF_MLME_TAG_CONN_IBSS)
    {
        MLME_INFO("seach ibss network...");
        mlme_set_state(pnic_info, MLME_STATE_IBSS_CONN_SCAN);
        pmlme_info->pscan_msg = pmsg;
        /*scanning*/
        PT_SPAWN(pt, pt_sub,
                 core_conn_scan_ibss_thrd(pt_sub, pnic_info,
                                          (void *)pmlme_info->pscan_msg->value,
                                          &reason));
        if (reason)
        {
            MLME_INFO("scan ibss network fall, error code: %d", reason);
        }

        /*join ibss*/
        MLME_INFO("join ibss...");
        param = (mlme_conn_ibss_t *)pmlme_info->pscan_msg->value;
        if (wf_adhoc_ibss_join(pnic_info, param->framework, reason))
        {
            wf_msg_del(pmsg_que, pmlme_info->pscan_msg);
            MLME_INFO("join ibss fall");

        }
        else
        {
            wf_msg_del(pmsg_que, pmlme_info->pscan_msg);
            wf_timer_set(&adhoc_info->timer, ADHOC_KEEPALIVE_TIMEOUT);

            /*keepalive & receive beacon*/
            PT_INIT(pt_sub);
            do
            {
                PT_YIELD(pt);
                if(pnic_info->is_surprise_removed || pnic_info->is_driver_stopped)
                {
                    PT_EXIT(pt);
                }

                wf_adhoc_keepalive_thrd(pnic_info);
            }
            while (PT_SCHEDULE(adhoc_conn_maintain_msg(pt_sub, pnic_info)));

            MLME_INFO("leave ibss role");
        }
    }
#endif

    else
    {
        MLME_INFO("drop unsuited message(tag: %d)", pmsg->tag);
        wf_msg_del(pmsg_que, pmsg);
    }

    mlme_set_state(pnic_info, MLME_STATE_IDLE);
    /* restart thread */
    PT_RESTART(pt);

    PT_END(pt);
}

wf_inline wf_bool mlme_can_run (nic_info_st *pnic_info)
{
    if (!pnic_info->is_up)
    {
        return wf_false;
    }

    {
        sys_work_mode_e work_mode = get_sys_work_mode(pnic_info);
        if (work_mode != WF_INFRA_MODE && work_mode != WF_ADHOC_MODE)
        {
            return wf_false;
        }
    }

    return wf_true;
}

static int mlme_core (nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;

    MLME_DBG();

    wf_os_api_thread_affinity(DEFAULT_CPU_ID);

    while (!WF_CANNOT_RUN(pnic_info))
    {
        if (!mlme_can_run(pnic_info))
        {
            wf_msleep(1);
            continue;
        }

        /* poll mlme core */
        pmlme_info->babort_thrd = wf_false;
        PT_INIT(&pmlme_info->pt[0]);
        while (PT_SCHEDULE(mlme_core_thrd(pnic_info)))
        {
            wf_msleep(1);
        }
    }

    MLME_DBG("wait for thread destory...");
    while (!wf_os_api_thread_wait_stop(pmlme_info->tid))
    {
        wf_msleep(1);
    }

    wf_os_api_thread_exit(pmlme_info->tid);

    return 0;
}

static int mlme_msg_send (nic_info_st *pnic_info,
                          wf_msg_tag_t tag, void *value, wf_u8 len)
{
    mlme_info_t *pmlme_info;
    wf_msg_que_t *pmsg_que;
    wf_msg_t *pmsg;
    int rst;

    if (pnic_info == NULL)
    {
        return -1;
    }

    pmlme_info = pnic_info->mlme_info;
    if (pmlme_info == NULL)
    {
        return -2;
    }

    pmsg_que = &pmlme_info->msg_que;
    rst = wf_msg_new(pmsg_que, tag, &pmsg);
    if (rst)
    {
        MLME_WARN("wf_msg_new fail error code: %d", rst);
        return -3;
    }
    if (value && len)
    {
        pmsg->len = len;
        wf_memcpy(pmsg->value, value, len);
    }

    rst = wf_msg_push(pmsg_que, pmsg);
    if (rst)
    {
        wf_msg_del(pmsg_que, pmsg);
        MLME_WARN("wf_msg_push fail error code: %d", rst);
        return -4;
    }

    return 0;
}

int wf_mlme_scan_start (nic_info_st *pnic_info, scan_type_e type,
                        wf_wlan_ssid_t ssids[], wf_u8 ssid_num,
                        wf_u8 chs[], wf_u8 ch_num,
                        wf_mlme_framework_e frm_work)
{
    mlme_info_t *pmlme_info;
    wf_msg_que_t *pmsg_que;
    wf_msg_t *pmsg;
    int rst;

    if (pnic_info == NULL)
    {
        return -1;
    }

    if (!pnic_info->is_up)
    {
        MLME_WARN("ndev is down");
        return -2;
    }

    MLME_DBG();

    pmlme_info = pnic_info->mlme_info;
    if (pmlme_info == NULL)
    {
        return -3;
    }

    pmsg_que = &pmlme_info->msg_que;
    rst = wf_msg_new(pmsg_que, WF_MLME_TAG_SCAN, &pmsg);
    if (rst)
    {
        MLME_WARN("msg new fail error code: %d", rst);
        return -4;
    }

    {
        mlme_scan_t *param = (mlme_scan_t *)pmsg->value;
        param->type = type;
        param->ssid_num = ssid_num;
        if (ssid_num)
        {
            wf_memcpy(param->ssids, ssids, ssid_num * sizeof(param->ssids[0]));
        }
        param->ch_num = ch_num;
        if (ch_num)
        {
            wf_memcpy(param->chs, chs, ch_num * sizeof(param->chs[0]));
        }
        param->framework = frm_work;
    }

    rst = wf_msg_push(pmsg_que, pmsg);
    if (rst)
    {
        wf_msg_del(pmsg_que, pmsg);
        MLME_WARN("msg push fail error code: %d", rst);
        return -5;
    }

    return 0;
}


int wf_mlme_scan_abort (nic_info_st *pnic_info)
{
    int rst;

    if (pnic_info == NULL)
    {
        return -1;
    }

    {
        sys_work_mode_e work_mode = get_sys_work_mode(pnic_info);
        if (work_mode != WF_INFRA_MODE)
        {
            return -2;
        }
    }

    {
        mlme_info_t *pmlme_info = pnic_info->mlme_info;
        if (!(pmlme_info && pmlme_info->pscan_msg &&
              pmlme_info->pscan_msg->tag == WF_MLME_TAG_SCAN))
        {
            return -3;
        }
    }

    MLME_DBG();

    rst = mlme_msg_send(pnic_info, WF_MLME_TAG_SCAN_ABORT, NULL, 0);
    if (rst)
    {
        return -4;
    }

    return 0;
}

int wf_mlme_conn_scan_rsp (nic_info_st *pnic_info,
                           wf_80211_mgmt_t *pmgmt, wf_u16 mgmt_len)
{
    mlme_info_t *pmlme_info;
    wf_msg_que_t *pmsg_que;
    wf_msg_t *pmsg;
    int rst;

    if (pnic_info == NULL || (pmgmt == NULL && mgmt_len == 0))
    {
        return -1;
    }

    if (!pnic_info->is_up)
    {
        MLME_WARN("ndev down");
        return -2;
    }

    if (!wf_is_scanning(pnic_info))
    {
        return -3;
    }
    pmlme_info = pnic_info->mlme_info;
    pmsg_que = &pmlme_info->msg_que;

    MLME_DBG();

    rst = wf_msg_new(pmsg_que, WF_MLME_TAG_SCAN_RSP, &pmsg);
    if (rst)
    {
        MLME_WARN("msg new fail error code: %d", rst);
        return -4;
    }
    /* copy frame */
    pmsg->len = mgmt_len;
    wf_memcpy(pmsg->value, pmgmt, mgmt_len);
    rst = wf_msg_push(pmsg_que, pmsg);
    if (rst)
    {
        wf_msg_del(pmsg_que, pmsg);
        MLME_WARN("msg push fail error code: %d", rst);
        return -5;
    }

    return 0;
}

int wf_mlme_conn_start (nic_info_st *pnic_info, wf_80211_bssid_t bssid,
                        wf_wlan_ssid_t *pssid, wf_mlme_framework_e frm_work,
                        wf_bool en_disconn_ind)
{
    mlme_info_t *pmlme_info;
    wf_msg_que_t *pmsg_que;
    wf_msg_t *pmsg;
    mlme_conn_t *param;
    int rst;

    if (pnic_info == NULL || (bssid == NULL && pssid == NULL))
    {
        return -1;
    }

    if (!pnic_info->is_up)
    {
        MLME_WARN("ndev down");
        return -2;
    }
    pmlme_info = pnic_info->mlme_info;
    pmsg_que = &pmlme_info->msg_que;

    MLME_DBG();

    if (pmlme_info->pconn_msg)
    {
        MLME_DBG("abort current connection");
        wf_mlme_conn_abort(pnic_info, wf_false);
    }
    /* delete existing connect message */
    if (!wf_msg_get(pmsg_que, &pmsg) && pmsg->tag == WF_MLME_TAG_CONN)
    {
        wf_msg_del(pmsg_que, pmsg);
    }

    rst = wf_msg_new(pmsg_que, WF_MLME_TAG_CONN, &pmsg);
    if (rst)
    {
        MLME_WARN("msg new fail error code: %d", rst);
        return -3;
    }
    param = (mlme_conn_t *)pmsg->value;

    param->en_disconn_ind = en_disconn_ind;
    /* set bssid */
    if (bssid)
    {
        wf_memcpy(param->bssid, bssid, sizeof(param->bssid));
    }
    else
    {
        wf_memset(param->bssid, 0, sizeof(param->bssid));
    }
    /* set ssid */
    if (pssid && pssid->length)
    {
        wf_memcpy(&param->ssid, pssid, sizeof(param->ssid));
        param->ssid.data[param->ssid.length] = '\0';
    }
    else
    {
        param->ssid.length = 0;
    }
    /* set framework */
    param->framework = frm_work;

    rst = wf_msg_push(pmsg_que, pmsg);
    if (rst)
    {
        wf_msg_del(pmsg_que, pmsg);
        MLME_WARN("msg push fail error code: %d", rst);
        return -4;
    }

    return 0;
}

int wf_mlme_conn_abort (nic_info_st *pnic_info, wf_bool en_disconn_ind)
{
    int rst;

    if (pnic_info == NULL)
    {
        MLME_ERROR("null point");
        return -1;
    }

    {
        sys_work_mode_e work_mode = get_sys_work_mode(pnic_info);
        if (work_mode != WF_INFRA_MODE)
        {
            MLME_WARN("unsuited role");
            return -2;
        }
    }

    {
        mlme_info_t *pmlme_info = pnic_info->mlme_info;
        if (!(pmlme_info && pmlme_info->pconn_msg &&
              pmlme_info->pconn_msg->tag == WF_MLME_TAG_CONN))
        {
            MLME_WARN("no connection abort");
            return -3;
        }
    }

    MLME_DBG();

    {
        mlme_conn_abort_t value =
        {
            .en_disconn_ind = en_disconn_ind,
        };
        rst = mlme_msg_send(pnic_info, WF_MLME_TAG_CONN_ABORT,
                            &value, sizeof(mlme_conn_abort_t));
        if (rst)
        {
            return -4;
        }
    }

    return 0;
}

int wf_mlme_deauth (nic_info_st *pnic_info, wf_bool en_disconn_ind)
{
    int rst;

    if (pnic_info == NULL)
    {
        return -1;
    }

    if (!pnic_info->is_up)
    {
        MLME_WARN("ndev down");
        return -2;
    }

    {
        sys_work_mode_e work_mode = get_sys_work_mode(pnic_info);
        if (work_mode != WF_INFRA_MODE)
        {
            MLME_WARN("unsuited role");
            return -3;
        }
    }

    MLME_DBG();

    {
        mlme_deauth_t value =
        {
            .en_disconn_ind = en_disconn_ind,
        };
        rst = mlme_msg_send(pnic_info, WF_MLME_TAG_DEAUTH,
                            &value, sizeof(mlme_deauth_t));
        if (rst)
        {
            return -4;
        }
    }

    return 0;
}

int wf_mlme_deassoc (nic_info_st *pnic_info)
{
    int rst;

    if (pnic_info == NULL)
    {
        return -1;
    }

    if (!pnic_info->is_up)
    {
        MLME_WARN("ndev down");
        return -2;
    }

    MLME_DBG();

    rst = mlme_msg_send(pnic_info, WF_MLME_TAG_DEASSOC, NULL, 0);
    if (rst)
    {
        return -3;
    }

    return 0;
}

int wf_mlme_add_ba_req (nic_info_st *pnic_info)
{
    int rst;

    if (pnic_info == NULL)
    {
        return -1;
    }

    if (!pnic_info->is_up)
    {
        MLME_WARN("ndev down");
        return -2;
    }

    MLME_DBG();

    rst = mlme_msg_send(pnic_info, WF_MLME_TAG_ADD_BA_REQ, NULL, 0);
    if (rst)
    {
        return -3;
    }

    return 0;
}

int wf_mlme_add_ba_rsp (nic_info_st *pnic_info)
{
    int rst;

    if (pnic_info == NULL)
    {
        return -1;
    }

    if (!pnic_info->is_up)
    {
        MLME_WARN("ndev down");
        return -2;
    }

    MLME_DBG();

    rst = mlme_msg_send(pnic_info, WF_MLME_TAG_ADD_BA_RSP, NULL, 0);
    if (rst)
    {
        return -3;
    }

    return 0;
}

int mlme_set_state (nic_info_st *pnic_info, mlme_state_e state)
{
    mlme_info_t *pmlme_info;
    wf_irq irq;

    if (pnic_info == NULL)
    {
        return -1;
    }

    pmlme_info = pnic_info->mlme_info;
    if (pmlme_info == NULL)
    {
        return -2;
    }

    wf_lock_irq_lock(&pmlme_info->state_lock, &irq);
    pmlme_info->state = state;
    wf_lock_irq_unlock(&pmlme_info->state_lock, &irq);

    return 0;
}

int wf_mlme_get_state (nic_info_st *pnic_info, mlme_state_e *state)
{
    mlme_info_t *pmlme_info;
    wf_irq irq;

    if (pnic_info == NULL)
    {
        return -1;
    }

    pmlme_info = pnic_info->mlme_info;
    if (pmlme_info == NULL)
    {
        return -2;
    }

    wf_lock_irq_lock(&pmlme_info->state_lock, &irq);
    *state = pmlme_info->state;
    wf_lock_irq_unlock(&pmlme_info->state_lock, &irq);

    return 0;
}

int wf_mlme_set_connect (nic_info_st *pnic_info, wf_bool bconnect)
{
    mlme_info_t *pmlme_info;

    if (pnic_info == NULL)
    {
        return -1;
    }

    pmlme_info = pnic_info->mlme_info;
    if (pmlme_info == NULL)
    {
        return -2;
    }

    wf_lock_spin_lock(&pmlme_info->connect_lock);
    pmlme_info->connect = bconnect;
    wf_lock_spin_unlock(&pmlme_info->connect_lock);

    return 0;
}

int wf_mlme_get_connect (nic_info_st *pnic_info, wf_bool *bconnect)
{
    mlme_info_t *pmlme_info;

    if (pnic_info == NULL)
    {
        return -1;
    }

    pmlme_info = pnic_info->mlme_info;
    if (pmlme_info == NULL)
    {
        return -2;
    }

    wf_lock_spin_lock(&pmlme_info->connect_lock);
    *bconnect = pmlme_info->connect;
    wf_lock_spin_unlock(&pmlme_info->connect_lock);

    return 0;
}

int wf_mlme_get_traffic_busy (nic_info_st *pnic_info, wf_bool *bbusy)
{
    mlme_info_t *pmlme_info;

    if (pnic_info == NULL || bbusy == NULL)
    {
        return -1;
    }

    pmlme_info = pnic_info->mlme_info;
    if (pmlme_info == NULL)
    {
        return -2;
    }

    *bbusy = pmlme_info->link_info.busy_traffic;

    return 0;
}

int wf_mlme_abort (nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    if(get_sys_work_mode(pnic_info) != WF_ADHOC_MODE)
    {
        wf_mlme_conn_abort(pnic_info, wf_true);
        wf_mlme_scan_abort(pnic_info);
    }

    pmlme_info->babort_thrd = wf_true;

    return 0;
}

#ifdef CFG_ENABLE_ADHOC_MODE
int wf_mlme_scan_ibss_start (nic_info_st *pnic_info,
                             wf_wlan_ssid_t *pssid,
                             wf_u8 *pch,
                             wf_mlme_framework_e frm_work)
{
    mlme_info_t *pmlme_info;
    wf_msg_que_t *pmsg_que;
    wf_msg_t *pmsg;
    int rst;

    if (pnic_info == NULL)
    {
        return -1;
    }

    if (!pnic_info->is_up)
    {
        MLME_WARN("ndev is down");
        return -2;
    }

    if (pssid == NULL || pch == NULL)
    {
        return -3;
    }

    MLME_DBG();

    pmlme_info = pnic_info->mlme_info;
    if (pmlme_info == NULL)
    {
        return -4;
    }

    pmsg_que = &pmlme_info->msg_que;
    rst = wf_msg_new(pmsg_que, WF_MLME_TAG_CONN_IBSS, &pmsg);
    if (rst)
    {
        MLME_WARN("msg new fail error code: %d", rst);
        return -5;
    }

    {
        mlme_conn_ibss_t *param = (mlme_conn_ibss_t *)pmsg->value;
        wf_memcpy(&param->ssid, pssid, sizeof(param->ssid));
        wf_memcpy(&param->ch, pch, sizeof(wf_u8));
        param->framework = frm_work;

    }


    rst = wf_msg_push(pmsg_que, pmsg);
    if (rst)
    {
        wf_msg_del(pmsg_que, pmsg);
        MLME_WARN("msg push fail error code: %d", rst);
        return -5;
    }

    return 0;
}
#endif

wf_inline static int mlme_msg_init (wf_msg_que_t *pmsg_que)
{
    wf_msg_init(pmsg_que);
    return (wf_msg_alloc(pmsg_que, WF_MLME_TAG_SCAN_ABORT, 0, 2) ||
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_CONN_ABORT, sizeof(mlme_conn_abort_t), 2) ||
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_DEAUTH, sizeof(mlme_deauth_t), 1) ||
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_DEASSOC, 0, 1) ||
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_SCAN, sizeof(mlme_scan_t), 2) ||
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_SCAN_RSP, sizeof(mlme_scan_rsp_t), 2) ||
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_CONN, sizeof(mlme_conn_t), 2) ||
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_ADD_BA_REQ, 0, 1) ||
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_ADD_BA_RSP, 0, 1) ||
#ifdef CFG_ENABLE_ADHOC_MODE
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_CONN_IBSS, sizeof(mlme_conn_ibss_t), 1) ||
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_IBSS_LEAVE, 0, 1) ||
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_IBSS_BEACON_FRAME, sizeof(beacon_frame_t), 1) ||
#endif
#ifdef CONFIG_LPS
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_LPS, 0, 1) ||
#endif
            wf_msg_alloc(pmsg_que, WF_MLME_TAG_KEEPALIVE, 0, 1)) ? -1 : 0;
}

wf_inline static int mlme_msg_deinit (wf_msg_que_t *pmsg_que)
{
    return wf_msg_free(pmsg_que);
}

int wf_mlme_init (nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info;

    MLME_DBG();

    pmlme_info = wf_kzalloc(sizeof(mlme_info_t));
    if (pmlme_info == NULL)
    {
        MLME_WARN("malloc mlme_info_t failed");
        return -1;
    }
    pnic_info->mlme_info = pmlme_info;

    wf_lock_spin_init(&pmlme_info->state_lock);
    wf_lock_spin_init(&pmlme_info->connect_lock);
    wf_mlme_set_connect(pnic_info, wf_false);
    pmlme_info->babort_thrd = wf_false;
    mlme_set_state(pnic_info, MLME_STATE_IDLE);
    if (mlme_msg_init(&pmlme_info->msg_que))
    {
        MLME_WARN("malloc msg init failed");
        return -2;
    }
    sprintf(pmlme_info->mlmeName,
            pnic_info->virNic ? "mlme_sm:vir%d_s%d" : "mlme_sm:wlan%d_s%d",
            pnic_info->hif_node_id, pnic_info->ndev_id);

    pmlme_info->tid = wf_os_api_thread_create(pmlme_info->tid,
                      pmlme_info->mlmeName,
                      mlme_core,
                      pnic_info);
    if (pmlme_info->tid == NULL)
    {
        MLME_WARN("create mlme thread failed");
        return -3;
    }
    wf_os_api_thread_wakeup(pmlme_info->tid);

    return 0;
}

int wf_mlme_term (nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info;

    MLME_DBG();
    pmlme_info = pnic_info->mlme_info;
    if (pmlme_info == NULL)
    {
        return 0;
    }

    wf_mlme_abort(pnic_info);

    MLME_DBG("destory thread");
    if (pmlme_info->tid)
    {
        wf_os_api_thread_destory(pmlme_info->tid);
        pmlme_info->tid = 0;
    }

    MLME_DBG("del msg que");
    mlme_msg_deinit(&pmlme_info->msg_que);

    MLME_DBG("del lock");
    wf_lock_spin_free(&pmlme_info->state_lock);
    wf_lock_spin_free(&pmlme_info->connect_lock);

    MLME_DBG("free pmlme_info");
    wf_kfree(pmlme_info);
    pnic_info->mlme_info = NULL;

    MLME_DBG("end");

    return 0;
}

