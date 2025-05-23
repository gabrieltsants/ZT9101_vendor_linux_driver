/*
 * adhoc.c
 *
 * used for AdHoc mode
 *
 * Author: luozhi
 *
 * Copyright (c) 2021 Shandong ZTop Microelectronics Co., Ltd
 *
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include "common.h"

#ifdef CFG_ENABLE_ADHOC_MODE

#define ADHOC_DBG(fmt, ...)      LOG_D("[%s][%d]"fmt, __func__, __LINE__, ##__VA_ARGS__)
#define ADHOC_ARRAY(data, len)   zt_log_array(data, len)
#define ADHOC_INFO(fmt, ...)     LOG_I("[%s][%d]"fmt, __func__, __LINE__, ##__VA_ARGS__)
#define ADHOC_WARN(fmt, ...)     LOG_W("[%s][%d]"fmt, __func__, __LINE__, ##__VA_ARGS__)
#define ADHOC_ERROR(fmt, ...)    LOG_E("[%s][%d]"fmt, __func__, __LINE__, ##__VA_ARGS__)


zt_s32 zt_adhoc_bcn_msg_send(nic_info_st *pnic_info, zt_80211_mgmt_t *pmgmt,
                             zt_u16 mgmt_len);
zt_s32 mlme_set_connect(nic_info_st *pnic_info, zt_bool bconnect);

zt_bool zt_get_adhoc_master(nic_info_st *pnic_info)
{
    zt_adhoc_info_t *adhoc_info = (zt_adhoc_info_t *)pnic_info->adhoc_info;
    return adhoc_info->adhoc_master;
}

zt_bool zt_set_adhoc_master(nic_info_st *pnic_info, zt_bool status)
{
    zt_adhoc_info_t *adhoc_info = (zt_adhoc_info_t *)pnic_info->adhoc_info;
    return adhoc_info->adhoc_master = status;
}

inline static zt_u32 zt_adhoc_andom32(void)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,5,0))
    return get_random_u32();
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(3,8,0))
    return prandom_u32();
#elif (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18))
    zt_u32 random_int;
    get_random_bytes(&random_int, 4);
    return random_int;
#else
    return random32();
#endif
}

static void zt_make_random_ibss(zt_u8 *pibss)
{
    *((zt_u32 *)(&pibss[2])) = zt_adhoc_andom32();
    pibss[0] = 0x02;
    pibss[1] = 0x11;
    pibss[2] = 0x22;
}

static zt_u8 adhoc_check_beacon(zt_wlan_network_t *pcur_network,
                                zt_80211_mgmt_t *pmgmt, zt_u16 mgmt_len)
{
    zt_80211_mgmt_ie_t *pie;
    zt_u8 *pies = &pmgmt->beacon.variable[0];
    zt_u16 ies_len = mgmt_len;

    if (zt_80211_hdr_type_get(pmgmt) != ZT_80211_FRM_BEACON)
    {
        return 0;
    }

    ADHOC_DBG();
    if (zt_80211_mgmt_ies_search(pies, ies_len, ZT_80211_MGMT_EID_SSID, &pie))
    {
        return 0;
    }

    if (pcur_network->ssid.length != pie->len ||
            zt_memcmp(pcur_network->ssid.data, pie->data, pie->len))
    {
        ADHOC_WARN("beacon frame no ssid element field");
        return 0;
    }

    if ((!ZT_80211_CAPAB_IS_IBSS(pmgmt->beacon.capab)
            && !zt_memcmp(pcur_network->bssid, pmgmt->bssid, ZT_80211_MAC_ADDR_LEN)))
    {
        return 0;
    }

    if (mgmt_len > ZT_80211_MGMT_BEACON_SIZE_MAX)
    {
        ADHOC_WARN("auth frame length too long");
        return 0;
    }

    return 1;
}

zt_s32 zt_adhoc_work(nic_info_st *pnic_info, zt_80211_mgmt_t *pmgmt,
                     zt_u16 mgmt_len)
{
    wdn_net_info_st *pwdn_info;
    zt_wlan_mgmt_info_t *pwlan_info = pnic_info->wlan_mgmt_info;

    if (zt_get_adhoc_master(pnic_info) == zt_false)
    {
        return -1;
    }

    if (!adhoc_check_beacon(&pwlan_info->cur_network, pmgmt, mgmt_len))
    {
        return -1;
    }

    pwdn_info = zt_wdn_find_info(pnic_info, pmgmt->sa);
    if (pwdn_info)
    {
        pwdn_info->rx_pkt_stat++;
        return 0;
    }

    if (zt_adhoc_bcn_msg_send(pnic_info,  pmgmt, mgmt_len))
    {
        ADHOC_WARN("beacon msg enque fail");
        return -2;
    }

    return 0;
}

void zt_adhoc_add_sta_ratid(nic_info_st *pnic_info, wdn_net_info_st *pwdn_info)
{
    ADHOC_DBG();

    if (pwdn_info->aid < 32)
    {
        zt_mcu_rate_table_update(pnic_info, pwdn_info);
    }
    else
    {
        ADHOC_ERROR("aid exceed the max number");
    }
}

zt_s32 zt_adhoc_new_boradcast_wdn(nic_info_st *pnic_info)
{
    wdn_net_info_st *pwdn_info;
    zt_adhoc_info_t *adhoc_info = pnic_info->adhoc_info;
    zt_u8 bc_addr[ZT_80211_MAC_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    pwdn_info = zt_wdn_add(pnic_info, bc_addr);
    if (pwdn_info == NULL)
    {
        ADHOC_WARN("alloc bmc wdn error");
        return -1;
    }

    adhoc_info->asoc_sta_count ++;
    pwdn_info->aid = 0;
    pwdn_info->qos_option = 0;
    pwdn_info->ieee8021x_blocked = zt_true;
    pwdn_info->mode = ZT_ADHOC_MODE;
    return 0;
}

void zt_adhoc_flush_wdn(nic_info_st *pnic_info)
{
    zt_list_t *pos, *pos_next;
    wdn_list *pwdn = (wdn_list *)pnic_info->wdn;
    zt_adhoc_info_t *adhoc_info = pnic_info->adhoc_info;
    wdn_net_info_st *pwdn_info;
    wdn_node_st *pwdn_node;

    zt_list_for_each_safe(pos, pos_next, &pwdn->head)
    {
        pwdn_node = zt_list_entry(pos, wdn_node_st, list);
        pwdn_info = &pwdn_node->info;
        if (pwdn_info->mode == ZT_ADHOC_MODE)
        {
            /* free the wdn */
            zt_wdn_remove(pnic_info, pwdn_info->mac);
        }
    }

    adhoc_info->asoc_sta_count = 0;
}

void zt_adhoc_flush_all_resource(nic_info_st *pnic_info,
                                 sys_work_mode_e network_type)
{
    zt_u32 tmp[2] = {0xAA, 0};
    zt_u32 val[2] = {0, 0};
    zt_adhoc_info_t *adhoc_info = pnic_info->adhoc_info;

    ADHOC_DBG();
    if (NULL == adhoc_info)
    {
        return;
    }

    /*flush wdn*/
    zt_adhoc_flush_wdn(pnic_info);

    if (!pnic_info->is_surprise_removed)
    {
        mcu_cmd_communicate(pnic_info, UMSG_OPS_HAL_SET_BCN, val, 2, NULL, 0);
        mcu_cmd_communicate(pnic_info, UMSG_OPS_HW_SET_MLME_DISCONNECT, tmp, 2,
                            NULL, 0);
        zt_mcu_set_bssid(pnic_info, NULL);
        switch (network_type)
        {
            case ZT_INFRA_MODE:
                zt_mcu_set_media_status(pnic_info, WIFI_FW_STATION_STATE);
                break;
            case ZT_ADHOC_MODE:
                zt_mcu_set_media_status(pnic_info, WIFI_FW_ADHOC_STATE);
                break;
#ifdef CFG_ENABLE_AP_MODE
            case ZT_MASTER_MODE:
                zt_mcu_set_media_status(pnic_info, WIFI_FW_AP_STATE);
                break;
#endif
            default:
                break;
        }
    }

    zt_os_api_ind_disconnect(pnic_info, adhoc_info->framework);
    zt_local_cfg_set_work_mode(pnic_info, network_type);
    if (!pnic_info->is_surprise_removed)
    {
        zt_mcu_set_op_mode(pnic_info, network_type);
    }

    zt_set_adhoc_master(pnic_info, zt_false);
}

zt_pt_ret_t zt_adhoc_prc_bcn(nic_info_st *pnic_info, zt_msg_t *pmsg,
                             wdn_net_info_st *pwdn_info)
{
    zt_wlan_mgmt_info_t *pwlan_info = pnic_info->wlan_mgmt_info;
    zt_wlan_network_t *pcur_network = &pwlan_info->cur_network;
    zt_80211_mgmt_t *pmgmt;
    zt_u16 var_len = 0, ofs;
    zt_u8 *pvar = NULL;
    zt_80211_mgmt_ie_t *pie;
    zt_u8 i, j;
    zt_80211_frame_e frame_type;

    ADHOC_DBG();
    if (pmsg == NULL)
    {

        ADHOC_WARN("MSG---IS---NULL-----");
        /* abort thread */
        return -1;
    }

    pmgmt = (zt_80211_mgmt_t *)pmsg->value;
    /* retrive frame type */
    frame_type = zt_80211_hdr_type_get(pmgmt);

    /* retrive element fiexd fields */
    pwdn_info->cap_info = zt_le16_to_cpu(pmgmt->beacon.capab);
    pwdn_info->bcn_interval = zt_le16_to_cpu(pmgmt->beacon.intv);

    /* initilize */
    // pwdn_info->rsn_pairwise_cipher = 0;
    pwdn_info->wmm_enable = zt_false;
    pwdn_info->wpa_enable = zt_false;
    pwdn_info->rsn_enable = zt_false;
    pwdn_info->datarate_len = 0;
    pwdn_info->ext_datarate_len = 0;
    pwdn_info->ssid_len = 0;

    /* element variable fields */
    if (frame_type == ZT_80211_FRM_BEACON)
    {
        pvar = pmgmt->beacon.variable;
        var_len = pmsg->len - ZT_OFFSETOF(zt_80211_mgmt_t, beacon.variable);
    }

    for (ofs = 0; ofs < var_len;
            ofs += ZT_OFFSETOF(zt_80211_mgmt_ie_t, data) + pie->len)
    {
        pie = (void *)&pvar[ofs];

        if (pie->element_id == ZT_80211_MGMT_EID_SUPP_RATES)
        {
            if (pie->len > ZT_ARRAY_SIZE(pwdn_info->datarate))
            {
                ADHOC_WARN("rates(%d) list size over limite", pie->len);

                return -1;
            }
            /* check and retrieve rate */
            zt_memset(pwdn_info->datarate, 0x0, sizeof(pwdn_info->datarate));
            for (i = 0; i < pie->len; i++)
            {
                for (j = 0; j < ZT_ARRAY_SIZE(pcur_network->rate); j++)
                {
                    if (pcur_network->rate[j] == 0x0)
                    {
                        break;
                    }
                    if (pie->data[i] == pcur_network->rate[j])
                    {
                        if (pwdn_info->datarate_len <
                                ZT_ARRAY_SIZE(pwdn_info->datarate))
                        {
                            pwdn_info->datarate[pwdn_info->datarate_len++] =
                                pie->data[i];
                            break;
                        }
                        else
                        {
                            ADHOC_WARN("beyond support rate upper limit");
                            /* abort thread */
                            break;
                        }
                    }
                }
            }
            if (pwdn_info->datarate_len == 0)
            {
                ADHOC_WARN("invalid support extend rates");
                return -1;
            }
            ADHOC_DBG("data rate(Mbps): %d, %d, %d, %d, %d, %d, %d, %d",
                      (pwdn_info->datarate[0] & 0x7F) / 2,
                      (pwdn_info->datarate[1] & 0x7F) / 2,
                      (pwdn_info->datarate[2] & 0x7F) / 2,
                      (pwdn_info->datarate[3] & 0x7F) / 2,
                      (pwdn_info->datarate[4] & 0x7F) / 2,
                      (pwdn_info->datarate[5] & 0x7F) / 2,
                      (pwdn_info->datarate[6] & 0x7F) / 2,
                      (pwdn_info->datarate[7] & 0x7F) / 2);
        }
        else if (pie->element_id == ZT_80211_MGMT_EID_EXT_SUPP_RATES)
        {
            if (pie->len > ZT_ARRAY_SIZE(pwdn_info->ext_datarate))
            {
                ADHOC_WARN("support extend rates(%d) list size over limite", pie->len);

                /* abort thread */
                return -1;
            }
            /* check and retrieve rate */
            zt_memset(pwdn_info->ext_datarate, 0x0,
                      ZT_ARRAY_SIZE(pwdn_info->ext_datarate));
            for (i = 0; i < pie->len; i++)
            {
                for (j = 0; j < ZT_ARRAY_SIZE(pcur_network->rate); j++)
                {
                    if (pcur_network->rate[j] == 0x0)
                    {
                        break;
                    }
                    if (pie->data[i] == pcur_network->rate[j])
                    {
                        if (pwdn_info->ext_datarate_len <
                                ZT_ARRAY_SIZE(pwdn_info->ext_datarate))
                        {
                            pwdn_info->ext_datarate[pwdn_info->ext_datarate_len++] =
                                pie->data[i];
                            break;
                        }
                        else
                        {
                            ADHOC_WARN("beyond support rate upper limit");

                            /* abort thread */
                            break;
                        }
                    }
                }
            }
            /* no find support ext rate */
            if (pwdn_info->ext_datarate_len == 0)
            {
                ADHOC_WARN("invalid support extend rates");

                /* abort thread */
                return -1;
            }
            ADHOC_DBG("extend data rate(Mbps): %d, %d, %d, %d",
                      (pwdn_info->ext_datarate[0] & 0x7F) / 2,
                      (pwdn_info->ext_datarate[1] & 0x7F) / 2,
                      (pwdn_info->ext_datarate[2] & 0x7F) / 2,
                      (pwdn_info->ext_datarate[3] & 0x7F) / 2);

        }

        else if (pie->element_id == ZT_80211_MGMT_EID_DS_PARAMS)
        {
            if (pie->data[0] == pcur_network->channel)
            {
                pwdn_info->channel = pie->data[0];
            }
        }
    }

    /* notify connection establish */
    ADHOC_DBG("prc  beacon end->"ZT_MAC_FMT, ZT_MAC_ARG(pwdn_info->mac));
    zt_adhoc_add_sta_ratid(pnic_info, pwdn_info);
    /* thread end */
    return 0;
}

void zt_adhoc_wdn_info_update(nic_info_st *pnic_info,
                              wdn_net_info_st *pwdn_info)
{
    zt_wlan_mgmt_info_t *pwlan_info = pnic_info->wlan_mgmt_info;
    zt_wlan_network_t *pcur_network = &pwlan_info->cur_network;

    /* generate wdn aid */
    pwdn_info->aid = pwdn_info->wdn_id + 1;
    /* retreive bssid into wdn_info */
    zt_memcpy(pwdn_info->bssid, pcur_network->bssid,
              ZT_ARRAY_SIZE(pwdn_info->bssid));
    /* reset connection rx packets statistics */
    pwdn_info->rx_pkt_stat_last  = 0;
    pwdn_info->rx_pkt_stat = 0;
    /* set mode */
    pwdn_info->mode = ZT_ADHOC_MODE;
    pwdn_info->ieee8021x_blocked = zt_true;
}

zt_s32 zt_adhoc_do_probrsp(nic_info_st *pnic_info,
                           zt_80211_mgmt_t *pframe, zt_u16 mgmt_len)
{
    struct xmit_buf *pxmit_buf;
    tx_info_st *ptx_info;
    zt_wlan_mgmt_info_t *pwlan_info = pnic_info->wlan_mgmt_info;
    hw_info_st *hw_info = pnic_info->hw_info;
    zt_wlan_network_t *pcur_network = &pwlan_info->cur_network;
    zt_80211_mgmt_t *pmgmt;
    zt_80211_mgmt_ie_t *pie;
    zt_u8 *pies = &pframe->probe_req.variable[0];
    zt_u16 ies_len = mgmt_len;

    if (zt_get_adhoc_master(pnic_info) == zt_false)
    {
        return 0;
    }

    if (zt_80211_mgmt_ies_search(pies, ies_len, ZT_80211_MGMT_EID_SSID, &pie))
    {
        ADHOC_WARN("no ssid element field");
        return -1;
    }

    if (pcur_network->ssid.length != pie->len ||
            zt_memcmp(pcur_network->ssid.data, pie->data, pie->len))
    {
        return -1;
    }

    if (!(zt_80211_is_same_addr(pframe->da, hw_info->macAddr) ||
            zt_80211_is_bcast_addr(pframe->da)))
    {
        ADHOC_WARN("probe request target address invalid");
        return -2;
    }

    /* alloc xmit_buf */
    ptx_info = (tx_info_st *)pnic_info->tx_info;
    pxmit_buf = zt_xmit_extbuf_new(ptx_info);
    if (pxmit_buf == NULL)
    {
        ADHOC_WARN("pxmit_buf is NULL");
        return -1;
    }

    /* clear frame head(txd + 80211head) */
    zt_memset(pxmit_buf->pbuf, 0,
              TXDESC_OFFSET + ZT_OFFSETOF(zt_80211_mgmt_t, beacon));

    /* set frame type */
    pmgmt = (void *)&pxmit_buf->pbuf[TXDESC_OFFSET];
    zt_80211_hdr_type_set(pmgmt, ZT_80211_FRM_PROBE_RESP);

    /* set address */
    zt_memcpy(pmgmt->da, pframe->sa, ZT_ARRAY_SIZE(pmgmt->da));
    zt_memcpy(pmgmt->sa, hw_info->macAddr, ZT_ARRAY_SIZE(pmgmt->sa));
    zt_memcpy(pmgmt->bssid, pcur_network->bssid, ZT_ARRAY_SIZE(pmgmt->bssid));

    /* set pies fiexd field */
    pmgmt->beacon.intv = zt_cpu_to_le16(pcur_network->bcn_interval);
    pmgmt->beacon.capab = zt_cpu_to_le16(pcur_network->cap_info);

    /* set pies variable fields */
    zt_memcpy(pmgmt->beacon.variable,
              pcur_network->ies, pcur_network->ies_length);

    /* send packet */
    pxmit_buf->pkt_len = ZT_OFFSETOF(zt_80211_mgmt_t, probe_resp.variable) +
                         pcur_network->ies_length;
    zt_nic_mgmt_frame_xmit(pnic_info, NULL, pxmit_buf, pxmit_buf->pkt_len);

    return 0;
}

zt_s32 proc_start_create_ibss_func(nic_info_st *pnic_info)
{
    zt_80211_mgmt_capab_t capab;
    zt_wlan_mgmt_info_t *pwlan_info = pnic_info->wlan_mgmt_info;
    zt_wlan_network_t *pcur_network = &pwlan_info->cur_network;

    zt_u8 null_addr[ZT_80211_MAC_ADDR_LEN] = { 0, 0, 0, 0, 0, 0 };
    struct beacon_ie *pbcn = (struct beacon_ie *)pcur_network->ies;
    capab = zt_le16_to_cpu(pbcn->capab);

    if (ZT_80211_CAPAB_IS_IBSS(capab))
    {
        // set media status
        zt_mcu_set_media_status(pnic_info, WIFI_FW_ADHOC_STATE & 03);

        if (pcur_network->ssid.data[0] && pcur_network->ssid.length)
        {
            if (zt_adhoc_send_beacon(pnic_info))
            {
                ADHOC_INFO("ADHOC send beacon fall!!!!");
                mlme_set_connect(pnic_info, zt_false);
                zt_mcu_set_mlme_join(pnic_info, 1);
                zt_mcu_set_bssid(pnic_info, null_addr);
                return -1;
            }
            else
            {
                zt_mcu_set_bssid(pnic_info, pwlan_info->cur_network.mac_addr);
                zt_mcu_set_mlme_join(pnic_info, 0);

                mlme_set_connect(pnic_info, zt_true);
            }
        }
    }

    return 0;
}

static void adhoc_init_reg(nic_info_st *pnic_info)
{
    zt_wlan_mgmt_info_t *pwlan_info = pnic_info->wlan_mgmt_info;
    zt_wlan_network_t *pcur_network = &pwlan_info->cur_network;

    ADHOC_DBG();

    /* set bssid */
    zt_mcu_set_bssid(pnic_info, (void *)pcur_network->bssid);

    zt_mcu_set_mlme_join(pnic_info, 0);

    /* set channel basebond */
    zt_hw_info_set_channel_bw(pnic_info,
                               pcur_network->channel, CHANNEL_WIDTH_20, HAL_PRIME_CHNL_OFFSET_DONT_CARE);

    /* set SISF */
    zt_mcu_set_sifs(pnic_info); /* 0x0808 -> for CCK, 0x0a0a -> for OFDM */

    /* set preamble */
    zt_mcu_set_preamble(pnic_info,
                        (pcur_network->cap_info & ZT_80211_MGMT_CAPAB_SHORT_PREAMBLE) ?
                        zt_true : zt_false);

    /* set slot time */
    zt_mcu_set_slot_time(pnic_info, pcur_network->short_slot);

    /* set ADHOC mode */
    zt_mcu_set_media_status(pnic_info, WIFI_FW_ADHOC_STATE);

    /* Set Security */
    zt_mcu_set_sec_cfg(pnic_info, 0xcf);
}

static void adhoc_update_reg(nic_info_st *pnic_info)
{
    zt_wlan_mgmt_info_t *pwlan_info = pnic_info->wlan_mgmt_info;
    zt_wlan_network_t *pcur_network = &pwlan_info->cur_network;
    zt_u16 br_cfg;

    /* set basic rate */
    get_bratecfg_by_support_dates(pcur_network->rate, pcur_network->rate_len,
                                  &br_cfg);
    zt_mcu_set_basic_rate(pnic_info, br_cfg);

    /* set beacon interval */
    zt_mcu_set_bcn_intv(pnic_info, pcur_network->bcn_interval * 100);

    /* set slot time */
    zt_mcu_set_slot_time(pnic_info, pcur_network->short_slot);

    /* set AC_PARAM */
    zt_mcu_set_ac_vo(pnic_info);
    zt_mcu_set_ac_vi(pnic_info);
    zt_mcu_set_ac_be(pnic_info);
    zt_mcu_set_ac_bk(pnic_info);

    zt_mcu_set_mlme_join(pnic_info, 2);
}

zt_u32 rate_len_to_get_func(zt_u8 *rateset)
{
    zt_u32 i = 0;
    while (1)
    {
        if ((rateset[i]) == 0)
        {
            break;
        }

        if (i > 12)
        {
            break;
        }

        i++;
    }
    return i;
}

void zt_adhoc_genarate_ie(nic_info_st *pnic_info)
{
    zt_wlan_mgmt_info_t *pwlan_info = pnic_info->wlan_mgmt_info;
    hw_info_st *hw_info = pnic_info->hw_info;
    zt_wlan_network_t *pcur_network = &pwlan_info->cur_network;
    const zt_u16 atimwindow = 0;
    zt_u32 sz = 0, rateLen;
    zt_u8 *ie = pcur_network->ies;

    ie = set_ie(ie, ZT_80211_MGMT_EID_SSID, pcur_network->ssid.length,
                pcur_network->ssid.data, &sz);

    rateLen = rate_len_to_get_func(hw_info->datarate);


    pcur_network->rate_len = rateLen;
    zt_memcpy(pcur_network->rate, hw_info->datarate, pcur_network->rate_len);

    if (rateLen > 8)
    {
        ie = set_ie(ie, ZT_80211_MGMT_EID_SUPP_RATES, 8,
                    hw_info->datarate, &sz);
    }
    else
    {
        ie = set_ie(ie, ZT_80211_MGMT_EID_SUPP_RATES, rateLen,
                    hw_info->datarate, &sz);
    }

    ie = set_ie(ie, ZT_80211_MGMT_EID_DS_PARAMS, 1,
                (zt_u8 *) & (pcur_network->channel), &sz);

    ie = set_ie(ie, ZT_80211_MGMT_EID_IBSS_PARAMS, 2,
                (zt_u8 *) & atimwindow, &sz);

    if (rateLen > 8)
    {
        ie = set_ie(ie, ZT_80211_MGMT_EID_EXT_SUPP_RATES, (rateLen - 8),
                    (hw_info->datarate + 8), &sz);
    }

    pcur_network->ies_length = sz;

}

zt_s32 zt_adhoc_send_beacon(nic_info_st *pnic_info)
{
    struct xmit_buf *pxmit_buf;
    tx_info_st *ptx_info;
    zt_wlan_mgmt_info_t *pwlan_info = pnic_info->wlan_mgmt_info;

    hw_info_st *hw_info = pnic_info->hw_info;
    zt_wlan_network_t *pcur_network = &pwlan_info->cur_network;
    zt_80211_mgmt_t *pmgmt;

    ptx_info = (tx_info_st *)pnic_info->tx_info;

    /* alloc xmit_buf */
    pxmit_buf = zt_xmit_extbuf_new(ptx_info);
    if (pxmit_buf == NULL)
    {
        ADHOC_WARN("pxmit_buf is NULL");
        return -1;
    }

    /* clear frame head(txd + 80211head) */
    zt_memset(pxmit_buf->pbuf, 0,
              TXDESC_OFFSET + ZT_OFFSETOF(zt_80211_mgmt_t, beacon));

    /* set frame type */
    pmgmt = (void *)&pxmit_buf->pbuf[TXDESC_OFFSET];
    zt_80211_hdr_type_set(pmgmt, ZT_80211_FRM_BEACON);

    /* set address */
    zt_memset(pmgmt->da, 0xff, ZT_80211_MAC_ADDR_LEN);
    zt_memcpy(pmgmt->sa, hw_info->macAddr, ZT_80211_MAC_ADDR_LEN);
    zt_memcpy(pmgmt->bssid, pcur_network->bssid, ZT_80211_MAC_ADDR_LEN);

    /* set ie fiexd field */
    pmgmt->beacon.intv = zt_cpu_to_le16(pcur_network->bcn_interval * 100);
    pmgmt->beacon.capab = zt_cpu_to_le16(pcur_network->cap_info);

    /* set ie variable fields */
    zt_memcpy(pmgmt->beacon.variable,
              pcur_network->ies, pcur_network->ies_length);

    /* send packet */
    pxmit_buf->pkt_len =
        ZT_OFFSETOF(zt_80211_mgmt_t, beacon.variable) + pcur_network->ies_length;

    ADHOC_DBG("xmit beacon!!!!");
    zt_nic_beacon_xmit(pnic_info, pxmit_buf, pxmit_buf->pkt_len);

    return 0;
}

zt_s32 zt_adhoc_ibss_join(nic_info_st *pnic_info, zt_u8 framework,
                          zt_s32 reason)
{
    zt_wlan_mgmt_info_t *pwlan_info = pnic_info->wlan_mgmt_info;
    zt_wlan_network_t *pcur_network = &pwlan_info->cur_network;
    zt_adhoc_info_t *adhoc_info = pnic_info->adhoc_info;
    wdn_net_info_st *pwdn_info = NULL;
    zt_u8 send_count = 10;
    zt_s32 rst;


    adhoc_info->framework = framework;

    if (zt_adhoc_new_boradcast_wdn(pnic_info))
    {
        ADHOC_WARN("create boardcast wdn_info fail");
    }

    if (reason == 0)
    {
        pwdn_info = zt_wdn_add(pnic_info, pcur_network->mac_addr);
        if (pwdn_info == NULL)
        {
            ADHOC_WARN("zt_proc_ibss_join new wdn fail");
            return -1;
        }
        adhoc_info->asoc_sta_count++;
        zt_adhoc_wdn_info_update(pnic_info, pwdn_info);
        zt_wdn_info_sta_update(pnic_info, pwdn_info);
        /*add sta ratid after wdn update*/
        zt_mcu_media_connect_set(pnic_info, pwdn_info, zt_true);
        zt_adhoc_add_sta_ratid(pnic_info, pwdn_info);
        zt_os_api_ind_connect(pnic_info, framework);
    }
    else
    {
        zt_make_random_ibss(pcur_network->bssid);
    }

    zt_adhoc_genarate_ie(pnic_info);
    adhoc_init_reg(pnic_info);

    /* enable data queue */
    zt_mcu_set_bcn_valid(pnic_info);
    zt_mcu_set_bcn_sel(pnic_info);
    while (send_count --)
    {
        rst = zt_adhoc_send_beacon(pnic_info);
        zt_yield();
    }
#if 0
    if (rst)
    {
        ADHOC_WARN("send beacon fail, error code: %d", rst);
        return -2;
    }
#endif
    adhoc_update_reg(pnic_info);
    zt_os_api_enable_all_data_queue(pnic_info->ndev);
    mlme_set_connect(pnic_info, zt_true);
    zt_set_adhoc_master(pnic_info, zt_true);
    return 0;
}

zt_s32 zt_adhoc_keepalive_thrd(nic_info_st *pnic_info)
{
    wdn_list *pwdn = (wdn_list *)pnic_info->wdn;
    zt_adhoc_info_t *adhoc_info = pnic_info->adhoc_info;
    zt_list_t *pos, *pos_next;
    wdn_node_st *pwdn_node;
    wdn_net_info_st *pwdn_info;

    if (NULL == adhoc_info || !zt_timer_expired(&adhoc_info->timer))
    {
        return 0;
    }

    zt_list_for_each_safe(pos, pos_next, &pwdn->head)
    {
        pwdn_node = zt_list_entry(pos, wdn_node_st, list);
        pwdn_info = &pwdn_node->info;
        if (pwdn_info->rx_pkt_stat_last != pwdn_info->rx_pkt_stat)
        {
            pwdn_info->rx_pkt_stat_last = pwdn_info->rx_pkt_stat;
            continue;
        }

        if (!zt_80211_is_bcast_addr(pwdn_info->mac))
        {
            /* free the wdn */

            adhoc_info->asoc_sta_count--;
            zt_wdn_remove(pnic_info, pwdn_info->mac);
            zt_mcu_media_connect_set(pnic_info, pwdn_info, zt_false);
            if (adhoc_info->asoc_sta_count == 1)
            {
                /*keep NL80211_IFTYPE_ADHOC type*/
                zt_os_api_cfg80211_unlink_ibss(pnic_info);
            }
        }
    }

    zt_timer_reset(&adhoc_info->timer);
    return 0;
}

zt_s32 zt_adhoc_leave_ibss_msg_send(nic_info_st *pnic_info)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;
    zt_msg_que_t *pmsg_que = &pmlme_info->msg_que;
    zt_msg_t *pmsg;
    zt_s32 rst;

    if (pnic_info == NULL)
    {
        return -1;
    }

    ADHOC_DBG();

    if (!pnic_info->is_up)
    {
        return -2;
    }

    rst = zt_msg_new(pmsg_que, ZT_MLME_TAG_IBSS_LEAVE, &pmsg);
    if (rst)
    {
        ADHOC_WARN("msg new fail error fail: %d", rst);
        return -4;
    }
    rst = zt_msg_push(pmsg_que, pmsg);
    if (rst)
    {
        zt_msg_del(pmsg_que, pmsg);
        ADHOC_WARN("msg push fail error fail: %d", rst);
        return -5;
    }

    return 0;
}

zt_s32 zt_adhoc_bcn_msg_send(nic_info_st *pnic_info, zt_80211_mgmt_t *pmgmt,
                             zt_u16 mgmt_len)
{
    mlme_info_t *pmlme_info = pnic_info->mlme_info;

    if (pnic_info == NULL || ZT_CANNOT_RUN(pnic_info))
    {
        return -1;
    }

    if (!pnic_info->is_up)
    {
        return -2;
    }

    ADHOC_DBG();

    /* new message information */
    {
        zt_msg_que_t *pmsg_que = &pmlme_info->msg_que;
        zt_msg_t *pmsg;
        zt_s32 rst;

        rst = zt_msg_new(pmsg_que, ZT_MLME_TAG_IBSS_BEACON_FRAME, &pmsg);
        if (rst)
        {
            ADHOC_WARN("msg new fail error fail: %d", rst);
            return -3;
        }
        pmsg->len = mgmt_len;
        zt_memcpy(pmsg->value, pmgmt, mgmt_len);
        rst = zt_msg_push(pmsg_que, pmsg);
        if (rst)
        {
            ADHOC_WARN("msg push fail error fail: %d", rst);
            return -4;
        }
    }

    return 0;
}

zt_s32 zt_adhoc_init(nic_info_st *pnic_info)
{
    zt_adhoc_info_t *adhoc_info;

    if (pnic_info == NULL)
    {
        return -1;
    }

    ADHOC_DBG();
    adhoc_info = zt_kzalloc(sizeof(zt_adhoc_info_t));

    if (adhoc_info == NULL)
    {
        ADHOC_WARN("malloc adhoc_info failed");
        return -2;
    }

    adhoc_info->asoc_sta_count = 0;
    zt_os_api_sema_init(&adhoc_info->sema, 1);
    pnic_info->adhoc_info = adhoc_info;
    zt_set_adhoc_master(pnic_info, zt_false);
    return 0;
}

zt_s32 zt_adhoc_term(nic_info_st *pnic_info)
{
    zt_adhoc_info_t *adhoc_info;

    if (pnic_info == NULL)
    {
        return -1;
    }

    ADHOC_DBG();
    adhoc_info = pnic_info->adhoc_info;

    if (adhoc_info)
    {
        zt_kfree(adhoc_info);
        pnic_info->adhoc_info = NULL;
    }

    return 0;
}

#endif
