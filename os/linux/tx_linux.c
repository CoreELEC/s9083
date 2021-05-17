#include <net/ieee80211_radiotap.h>
#include "ndev_linux.h"
#include "wf_os_api.h"
#include "wf_debug.h"
#include "hif.h"
#include "tx.h"
#include "common.h"

static wf_bool mpdu_send_complete_cb(nic_info_st *nic_info, struct xmit_buf *pxmitbuf)
{
    tx_info_st *tx_info = nic_info->tx_info;

    wf_xmit_buf_delete(tx_info, pxmitbuf);

    tx_work_wake(nic_info->ndev);

    return wf_true;
}

#ifdef CONFIG_SOFT_TX_AGGREGATION
static int  mpdu_agg_insert_sending_queue(nic_info_st *nic_info, struct xmit_buf *pxmitbuf)
{
    wf_u32 ff_hwaddr;
    int ret = WF_RETURN_FAIL;
    tx_info_st *tx_info         = NULL;

    if(NULL == nic_info)
    {
        LOG_E("[%s,%d] nic_info null", __func__, __LINE__);
        return WF_RETURN_FAIL;
    }

    if(NULL == pxmitbuf)
    {
        LOG_E("[%s,%d] pxmitbuf null", __func__, __LINE__);
        return WF_RETURN_FAIL;
    }

    tx_info = nic_info->tx_info;

    wf_tx_agg_num_fill(pxmitbuf->agg_num, pxmitbuf->pbuf);

    ret = wf_io_write_data(nic_info, pxmitbuf->agg_num, pxmitbuf->pbuf, pxmitbuf->pkt_len, pxmitbuf->ff_hwaddr, (void *)mpdu_send_complete_cb, nic_info, pxmitbuf);

    return ret;
}



static int tx_work_mpdu_xmit_agg(nic_info_st *nic_info)
{
    struct xmit_frame *pxframe  = NULL;
    tx_info_st *tx_info         = nic_info->tx_info;
    struct xmit_buf *pxmitbuf   = NULL;
    mlme_state_e state;
    mlme_info_t *mlme_info     = nic_info->mlme_info;
    wf_s32 res                  = 0;
    int addbaRet                = -1;
    wf_u8 pre_qsel = 0xFF;
    wf_u8 next_qsel = 0xFF;
    int packet_index       = 0;
    wf_u32 txlen   = 0;
    hw_info_st *hw_info = nic_info->hw_info;
    struct sk_buff *skb;

    while( (NULL != (pxframe = wf_tx_data_dequeue(tx_info))) && pxframe->pkt)
    {
        if(WF_CANNOT_RUN(nic_info))
        {
            return -1;
        }

        wf_mlme_get_state(nic_info, &state);
        if (state == MLME_STATE_SCAN || (state == MLME_STATE_ADD_BA_REQ) || (state == MLME_STATE_ADD_BA_RESP))
        {
            LOG_I("[%s] <1> mlme state:%d",__func__,state);
            if(pxmitbuf)
            {
                wf_xmit_buf_delete(tx_info, pxmitbuf);
                pxmitbuf = NULL;
            }

            wf_tx_data_enqueue_head(tx_info,pxframe);
            break;
        }

        if(NULL == pxmitbuf)
        {
            pxmitbuf = wf_xmit_buf_new(tx_info);
            if (pxmitbuf == NULL)
            {
                //LOG_W("[%s,%d] pxmitbuf is null",__func__,__LINE__);
                wf_tx_data_enqueue_head(tx_info,pxframe);
                break;
            }

        }

        if (pxframe->priority > 15)
        {
            LOG_E("[%s] priority:%d",__func__,pxframe->priority);
            if(pxmitbuf)
            {
                wf_xmit_buf_delete(tx_info, pxmitbuf);
                pxmitbuf = NULL;
            }
            //wf_tx_data_enqueue_head(tx_info,pxframe); this is wrong frame, drop it .
            break;
        }

        //LOG_I("[%d] buffer_addr:%p, [%d] frame_addr:%p, packet_index:%d",pxmitbuf->buffer_id,pxmitbuf, pxframe->frame_id ,pxframe, packet_index);

        if (mlme_info->link_info.num_tx_ok_in_period_with_tid[pxframe->qsel] > 100  && (hw_info->ba_enable == wf_true))
        {
            addbaRet = wf_action_frame_add_ba_request(nic_info, pxframe);
            if (addbaRet == 0)
            {
                if(pxmitbuf)
                {
                    wf_xmit_buf_delete(tx_info, pxmitbuf);
                    pxmitbuf = NULL;
                }

                wf_tx_data_enqueue_head(tx_info,pxframe);
                break;
            }
        }

        next_qsel = pxframe->qsel;
        if((pxmitbuf->pkt_len+ wf_get_wlan_pkt_size(pxframe) > wf_nic_get_tx_max_len(nic_info,pxframe))
            || (0 != packet_index && WF_RETURN_FAIL == wf_nic_tx_qsel_check(pre_qsel,next_qsel))
            #ifdef CONFIG_RICHV200
            || ((pxmitbuf->pkt_len <= 512) && (3 <= packet_index ) && (NIC_USB == nic_info->nic_type) ) //redmine/issues/45
            || ( (pxmitbuf->pkt_len%16 == 8) && (NIC_USB == nic_info->nic_type)) //redmine/issues/46
            #endif
            || MAX_AGG_NUM/2 <= packet_index )
        {
            #ifdef CONFIG_RICHV200
            if(((pxmitbuf->pkt_len <= 512) && (3 <= packet_index )) )
            {
                LOG_I("agg <<=512> pkt_len:%d,packet_index:%d",pxmitbuf->pkt_len,packet_index);
            }
            else if(pxmitbuf->pkt_len%16 == 8 )
            {
                LOG_I("agg<mod16==8> pkt_len:%d,packet_index:%d",pxmitbuf->pkt_len,packet_index);
            }
            else
            {
                //LOG_I("agg pkt_len:%d,packet_index:%d",pxmitbuf->pkt_len,packet_index);
            }
            #endif
            wf_tx_data_enqueue_head(tx_info,pxframe);
            break;
        }

        pxframe->pxmitbuf   = pxmitbuf;
        pxframe->buf_addr   = pxmitbuf->ptail;
        pre_qsel = pxframe->qsel;
        pxmitbuf->encrypt_algo = pxframe->encrypt_algo;
        pxmitbuf->qsel         = pxframe->qsel;
        skb = (struct sk_buff *)pxframe->pkt;
        res = wf_tx_msdu_to_mpdu(nic_info, pxframe, skb->data, skb->len);
        pxframe->agg_num = 1;
        wf_tx_txdesc_init(pxframe, pxframe->buf_addr, pxframe->last_txcmdsz,wf_true,1);

        txlen = pxframe->last_txcmdsz;

        /* data encrypt */
        if (wf_sec_encrypt(pxframe, pxframe->buf_addr, TXDESC_SIZE + txlen))
        {
            LOG_E("encrypt fail!!!!!!!!!!!");
        }

        wf_tx_stats_cnt(nic_info, pxframe, txlen);

        pxmitbuf->pg_num   += (TXDESC_SIZE+txlen+127)/128;
        pxmitbuf->ptail   += (TXDESC_SIZE+WF_RND_MAX(txlen, 8));
        pxmitbuf->pkt_len = pxmitbuf->pkt_len + TXDESC_SIZE + WF_RND_MAX(txlen, 8);

        #if TX_AGG_QUEUE_ENABLE
        wf_tx_agg_enqueue_head(tx_info, pxframe);
        #else
        wf_xmit_frame_enqueue(tx_info, pxframe);
        #endif
        packet_index++;
    }

    if (pxmitbuf && pxmitbuf->pkt_len > 0)
    {
        pxmitbuf->agg_num = packet_index;
        pxmitbuf->ff_hwaddr = wf_quary_addr(pre_qsel);
        wf_timer_set(&pxmitbuf->time, 0);

       //LOG_I("[%s,%d] buffer_id:%d,pg_num:%d,packet_index:%d,pkt_len:%d",__func__,__LINE__, (int)pxmitbuf->buffer_id,pxmitbuf->pg_num,packet_index,pxmitbuf->pkt_len);
        res = mpdu_agg_insert_sending_queue(nic_info, pxmitbuf);
        if(WF_RETURN_OK != res)
        {
            LOG_E("[%s,%d] mpdu_agg_insert_sending_queue failed",__func__,__LINE__);
            wf_xmit_buf_delete(tx_info, pxmitbuf);
        }
        #if TX_AGG_QUEUE_ENABLE
        {
            struct xmit_frame *tmp_frame  = NULL;
            while( (NULL != (tmp_frame = wf_tx_agg_dequeue(tx_info))))
            {

                wf_xmit_frame_enqueue(tx_info, tmp_frame);
            }
        }
        #endif
    }
    else
    {
        #if TX_AGG_QUEUE_ENABLE
        {
            struct xmit_frame *tmp_frame  = NULL;
            while( (NULL != (tmp_frame = wf_tx_agg_dequeue(tx_info))))
            {
                wf_tx_data_enqueue_head(tx_info, tmp_frame);
            }
        }
        #endif
    }

    return 0;
}

#else

static wf_bool mpdu_insert_sending_queue(nic_info_st *nic_info, struct xmit_frame *pxmitframe, wf_bool ack)
{
    wf_u8 *mem_addr;
    wf_u32 ff_hwaddr;
    wf_bool bRet = wf_true;
    int ret;
    wf_bool inner_ret = wf_true;
    wf_bool blast = wf_false;
    int t, sz, w_sz, pull = 0;
    struct xmit_buf *pxmitbuf = pxmitframe->pxmitbuf;
    hw_info_st *hw_info = nic_info->hw_info;
    wf_u32  txlen = 0;

    mem_addr = pxmitframe->buf_addr;

    for (t = 0; t < pxmitframe->nr_frags; t++)
    {
        if (inner_ret != wf_true && ret == wf_true)
            ret = wf_false;

        if (t != (pxmitframe->nr_frags - 1))
        {
            LOG_D("pxmitframe->nr_frags=%d\n", pxmitframe->nr_frags);
            sz = hw_info->frag_thresh;
            sz = sz - 4 - 0; /* 4: wlan head filed????????? */
        }
        else
        {
            /* no frag */
            blast = wf_true;
            sz = pxmitframe->last_txcmdsz;
        }

        pull = wf_tx_txdesc_init(pxmitframe, mem_addr, sz, wf_false, 1);
        if (pull)
        {
            mem_addr += PACKET_OFFSET_SZ; /* pull txdesc head */
            pxmitframe->buf_addr = mem_addr;
            w_sz = sz + TXDESC_SIZE;
        }
        else
        {
            w_sz = sz + TXDESC_SIZE + PACKET_OFFSET_SZ;
        }

        if (wf_sec_encrypt(pxmitframe, mem_addr, w_sz))
        {
            ret = wf_false;
            LOG_E("encrypt fail!!!!!!!!!!!");
        }
        ff_hwaddr = wf_quary_addr(pxmitframe->qsel);

        txlen = TXDESC_SIZE + pxmitframe->last_txcmdsz;
        pxmitbuf->pg_num   += (txlen+127)/128;
        wf_timer_set(&pxmitbuf->time, 0);

        if(blast)
        {
            ret = wf_io_write_data(nic_info, 1, mem_addr, w_sz,
                                          ff_hwaddr,(void *)mpdu_send_complete_cb, nic_info, pxmitbuf);
        }
        else
        {
            ret = wf_io_write_data(nic_info, 1, mem_addr, w_sz,
                                          ff_hwaddr, NULL, nic_info, pxmitbuf);
        }

        if (WF_RETURN_FAIL == ret)
        {
            bRet = wf_false;
            break;
        }

        wf_tx_stats_cnt(nic_info, pxmitframe, sz);

        mem_addr += w_sz;
        mem_addr = (wf_u8 *) WF_RND4(((SIZE_PTR) (mem_addr)));
    }

    return bRet;
}



static int tx_work_mpdu_xmit(nic_info_st *nic_info)
{
    struct xmit_frame *pxframe = NULL;
    tx_info_st *tx_info = nic_info->tx_info;
    struct xmit_buf *pxmitbuf = NULL;
    wf_s32 res = wf_false;
    wf_bool bTxQueue_empty;
    mlme_state_e state = MLME_STATE_IDLE;
    int addbaRet = -1;
    wf_bool bRet = wf_false;
    mlme_info_t *mlme_info = nic_info->mlme_info;
    hw_info_st *hw_info = nic_info->hw_info;
    struct sk_buff *skb;

    while (1)
    {
        if(WF_CANNOT_RUN(nic_info))
        {
            return -1;
        }

        wf_mlme_get_state(nic_info, &state);
        if (state == MLME_STATE_SCAN)
        {
            break;
        }

        bTxQueue_empty = wf_que_is_empty(&tx_info->pending_frame_queue);
        if (bTxQueue_empty == wf_true)
            break;

        pxmitbuf = wf_xmit_buf_new(tx_info);
        if (pxmitbuf == NULL)
        {
            break;
        }

        pxframe = wf_tx_data_getqueue(tx_info);
        if (pxframe)
        {
            pxframe->pxmitbuf = pxmitbuf;
            pxframe->buf_addr = pxmitbuf->pbuf;
            pxmitbuf->priv_data = pxframe;

            /* error msdu */
            if (pxframe->priority > 15)
            {
                wf_xmit_buf_delete(tx_info, pxmitbuf);
                wf_xmit_frame_delete(tx_info, pxframe);
                dev_kfree_skb_any(pxframe->pkt);
                pxframe->pkt = NULL;
                LOG_E("[%s]:error msdu", __func__);
                break;
            }

            /* BA start check */
            if (mlme_info->link_info.num_tx_ok_in_period_with_tid[pxframe->qsel] > 100 && (hw_info->ba_enable == wf_true))
            {
                addbaRet = wf_action_frame_add_ba_request(nic_info, pxframe);
                if (addbaRet == 0)
                {
                    LOG_I("Send Msg to MLME for starting BA!!");
                    wf_xmit_buf_delete(tx_info, pxmitbuf);
                    break;
                }
            }

            /* msdu2mpdu */
            if (pxframe->pkt != NULL)
            {
                skb = (struct sk_buff *)pxframe->pkt;

                res = wf_tx_msdu_to_mpdu(nic_info, pxframe, skb->data, skb->len);

                dev_kfree_skb_any(pxframe->pkt);
                pxframe->pkt = NULL;
            }

            /* send to hif tx queue */
            if (res == wf_true)
            {
                bRet = mpdu_insert_sending_queue(nic_info, pxframe, wf_false);
                if (bRet == wf_false)
                {
                    wf_xmit_buf_delete(tx_info, pxmitbuf);
                }
                else
                {
                    wf_xmit_frame_delete(tx_info, pxframe);
                }
            }
            else
            {
                LOG_E("wf_tx_msdu_to_mpdu error!!");

                wf_xmit_buf_delete(tx_info, pxmitbuf);
                wf_xmit_frame_delete(tx_info, pxframe);
            }

            /* check tx resource */
            bRet = wf_need_wake_queue(nic_info);
            if (bRet == wf_true)
            {
                ndev_tx_resource_enable(nic_info->ndev, pxframe->pkt);
            }
        }
        else
        {
            wf_xmit_buf_delete(tx_info, pxmitbuf);
            break;
        }
    }

    return 0;
}
#endif


void tx_work_init(struct net_device *ndev)
{
    ndev_priv_st *ndev_priv;
    nic_info_st *nic_info;

    ndev_priv = netdev_priv(ndev);
    nic_info = ndev_priv->nic;

#ifdef CONFIG_SOFT_TX_AGGREGATION
    tasklet_init(&ndev_priv->get_tx_data_task, (void (*)(unsigned long))tx_work_mpdu_xmit_agg,(unsigned long)nic_info);
#else
    tasklet_init(&ndev_priv->get_tx_data_task, (void (*)(unsigned long))tx_work_mpdu_xmit,(unsigned long)nic_info);
#endif
}

void tx_work_term(struct net_device *ndev)
{
    ndev_priv_st *ndev_priv;
    nic_info_st *nic_info;
    tx_info_st *tx_info;
    struct xmit_frame *pxmitframe;

    ndev_priv = netdev_priv(ndev);
    nic_info = ndev_priv->nic;
    tx_info = nic_info->tx_info;

    while (wf_que_is_empty(&tx_info->pending_frame_queue) == wf_false)
    {
        pxmitframe = wf_tx_data_getqueue(tx_info);
        wf_xmit_frame_delete(tx_info, pxmitframe);
        if (pxmitframe->pkt)
        {
            dev_kfree_skb_any(pxmitframe->pkt);
            pxmitframe->pkt = NULL;
        }
    }

    tasklet_kill(&ndev_priv->get_tx_data_task);
}

void tx_work_wake(struct net_device *ndev)
{
    ndev_priv_st *ndev_priv;

    ndev_priv = netdev_priv(ndev);

    tasklet_hi_schedule(&ndev_priv->get_tx_data_task);
}


