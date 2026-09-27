/* bb_client.h - minimal client-side interface to the AR8030 baseband SDK.
 *
 * fpvOS does not ship the vendor SDK. This header declares only the subset
 * of the libar8030_client.so ABI that kestrel actually calls: device
 * discovery, host connection, the ioctl request channel, and the data
 * sockets. Struct layouts and request codes are interoperability facts,
 * documented from observed wire behavior;
 * the library itself is extracted from your own device firmware by
 * scripts/extract-vendor.py.
 */
#ifndef FPVOS_BB_CLIENT_H
#define FPVOS_BB_CLIENT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BB_REQ_CFG 0
#define BB_REQ_GET 1
#define BB_REQ_SET 2
#define BB_REQ_CB 3
#define BB_REQ_SOCKET 4
#define BB_REQ_DBG 5
#define BB_REQ_REMOTE 6
#define BB_REQ_RPC 10
#define BB_REQ_RPC_IOCTL 11
#define BB_REQ_PLAT_CTL 12

#ifndef AR8030_API
#define AR8030_API
#endif

/* Opaque handles owned by the client library. */
typedef struct bb_dev_t bb_dev_t;
#define BB_CONFIG_MAX_CHAN_NUM 32       
/* 100, not 5. The daemon's cmd/length table (embedded in ar_ldy_gnd) gives
 * BB_SET_CANDIDATES an input length of 402 = 1 (slot) + 1 (mac_num) + 100*4,
 * and stock's own save path walks bb_mac_addr_0..99 in /factory/user_cfg.json.
 * At 5 we were sending a 22-byte struct where the daemon expects 402. */
#define BB_CONFIG_MAX_SLOT_CANDIDATE                                           \
  100
#define BB_BLACK_LIST_SIZE 3  
#define BB_CFG_DISTC                                                           \
  BB_REQUEST(BB_REQ_CFG, 7) 
#define BB_DEINIT_REQ BB_REQUEST(BB_REQ_RPC_IOCTL, 3)
#define BB_GET_1V1_INFO                                                        \
  BB_REQUEST(BB_REQ_GET, 107) 
#define BB_GET_AP_TIME BB_REQUEST(BB_REQ_GET, 12) 
#define BB_GET_CHAN_INFO                                                       \
  BB_REQUEST(BB_REQ_GET,                                                       \
             10) 
#define BB_GET_DISTC_RESULT                                                    \
  BB_REQUEST(BB_REQ_GET, 5) 
#define BB_GET_MCS                                                             \
  BB_REQUEST(BB_REQ_GET, 6) 
#define BB_GET_PAIR_RESULT                                                     \
  BB_REQUEST(BB_REQ_GET, 1) 
#define BB_GET_STATUS                                                          \
  BB_REQUEST(BB_REQ_GET, 0) 
#define BB_INIT_REQ BB_REQUEST(BB_REQ_RPC_IOCTL, 2)
#define BB_MAC_LEN 4          
#define BB_REQUEST(type, order)                                                \
  ((type) << 24 | (order))                 
#define BB_REQ_CFG 0        
#define BB_REQ_GET 1        
#define BB_REQ_SET 2        
#define BB_SET_AP_MAC                                                          \
  BB_REQUEST(BB_REQ_SET, 3) 
#define BB_SET_BANDWIDTH                                                       \
  BB_REQUEST(BB_REQ_SET, 22) 
#define BB_SET_CANDIDATES                                                      \
  BB_REQUEST(BB_REQ_SET, 4) 
#define BB_SET_CHAN BB_REQUEST(BB_REQ_SET, 6) 
#define BB_SET_CHAN_MODE                                                       \
  BB_REQUEST(BB_REQ_SET, 5)                   
#define BB_SET_EVENT_SUBSCRIBE                                                 \
  BB_REQUEST(BB_REQ_SET, 0) 
#define BB_SET_FREQ                                                            \
  BB_REQUEST(BB_REQ_SET, 105) 
#define BB_SET_LNA_MODE BB_REQUEST(BB_REQ_SET, 28) 
#define BB_SET_MCS                                                             \
  BB_REQUEST(BB_REQ_SET, 13) 
#define BB_SET_MCS_MODE                                                        \
  BB_REQUEST(BB_REQ_SET, 12) 
#define BB_SET_PAIR_MODE                                                       \
  BB_REQUEST(BB_REQ_SET, 2) 
#define BB_SET_POWER BB_REQUEST(BB_REQ_SET, 8) 
#define BB_SET_POWER_AUTO                                                      \
  BB_REQUEST(BB_REQ_SET, 9) 
#define BB_SET_PRJ_DISPATCH                                                    \
  BB_REQUEST(BB_REQ_SET, 200) 
#define BB_SET_WORK_CHAN_LIST 0x02000023  
#define BB_SOCK_FLAG_RX                                                        \
  (1 << 0) 
#define BB_SOCK_FLAG_TX                                                        \
  (1 << 1) 
#define BB_START_REQ BB_REQUEST(BB_REQ_RPC_IOCTL, 0)
#define BB_STOP_REQ BB_REQUEST(BB_REQ_RPC_IOCTL, 1)

typedef enum {
  BB_USER_0 = 0, 
  BB_USER_1,     
  BB_USER_2,     
  BB_USER_3,     
  BB_USER_4,     
  BB_USER_5,     
  BB_USER_6,     
  BB_USER_7,     
#if 0
    BB_USER_DFS = BB_USER_7,                                    
#endif
  BB_USER_BR_CS,    
  BB_USER_BR2_CS2,  
  BB_DATA_USER_MAX, 
  BB_USER_SWEEP = BB_DATA_USER_MAX, 
  BB_USER_SWEEP_SHORT,              
  BB_USER_MAX
} bb_user_e;

typedef enum {
  BB_BW_1_25M = 0, 
  BB_BW_2_5M = 1,  
  BB_BW_5M = 2,    
  BB_BW_10M = 3,   
  BB_BW_20M = 4,   
  BB_BW_40M = 5,   
  BB_BW_MAX
} bb_bandwidth_e;

typedef enum {
  BB_DIR_TX, 
  BB_DIR_RX, 
  BB_DIR_MAX
} bb_dir_e;

typedef enum {
  BB_EVENT_LINK_STATE = 0, 
  BB_EVENT_MCS_CHANGE,     
  BB_EVENT_CHAN_CHANGE,    
  BB_EVENT_PLOT_DATA,      
  BB_EVENT_FRAME_START,    
  BB_EVENT_OFFLINE,      
  BB_EVENT_PRJ_DISPATCH, 
  BB_EVENT_PAIR_RESULT,  
  BB_EVENT_PRJ_DISPATCH2,      
  BB_EVENT_MCS_CHANGE_END,     
  BB_EVENT_PRJ_DISPATCH2_UART, 
  BB_EVENT_PRJ_DISPATCH2_SDIO, 
  BB_EVENT_MAX
} bb_event_e;

typedef enum {
  BB_ROLE_AP,  
  BB_ROLE_DEV, 
  BB_ROLE_MAX
} bb_role_e;

typedef enum {
  BB_SLOT_0 = 0,  
  BB_SLOT_AP = 0, 
  BB_SLOT_1,      
  BB_SLOT_2,      
  BB_SLOT_3,      
  BB_SLOT_4,      
  BB_SLOT_5,      
  BB_SLOT_6,      
  BB_SLOT_7,      
  BB_SLOT_MAX
} bb_slot_e;

typedef struct {
  uint8_t addr[BB_MAC_LEN]; 
} bb_mac_t;

typedef struct {
  uint8_t
      slot;        
  uint8_t mac_num; 
  bb_mac_t mac_tab[BB_CONFIG_MAX_SLOT_CANDIDATE]; 
} bb_conf_candidates_t;

typedef struct {
  uint8_t pwr_auto; 
} bb_get_pwr_auto_out_t;

typedef struct {
  uint8_t usr; 
  uint8_t pwr; 
} bb_get_cur_pwr_out_t;

typedef void (*bb_event_callback)(
    void *arg,
    void *
        user);

typedef struct {
  uint8_t enable;  
  uint8_t window;  
  uint8_t timeout; 
  uint32_t
      offset; 
} bb_conf_distc_t;

typedef struct bb_dev_handle_t
    bb_dev_handle_t;

typedef struct bb_dev_t *bb_dev_list_t;

typedef struct {
  uint8_t frame_num; 
  uint8_t rsv[3];
} bb_get_1v1_info_in_t;

typedef struct {
  uint16_t chan_snr;
  uint8_t gain_a;
  uint8_t gain_b;
} bb_lfs_quality_t;

typedef struct {
  uint16_t snr; 
  uint16_t
      ldpc_tlv_err_ratio; 
  uint16_t
      ldpc_num_err_ratio;         
  uint8_t gain_a;                 
  uint8_t gain_b;                 
  uint8_t tx_mcs;                 
  uint8_t tx_chan;                
  uint8_t tx_power;               
  uint8_t lna_inner_bypass : 1;   
  uint8_t lna_fem_bypass : 1;     
  uint8_t rf_1tx : 1;             
  uint32_t tx_freq_khz;           
  bb_lfs_quality_t lfs_low_band;  
  bb_lfs_quality_t lfs_high_band; 
  uint8_t main_loc;               
  uint8_t rev[47];
} bb_info_t;

typedef struct {
  bb_info_t self; 
  bb_info_t peer; 
  uint8_t rev[64];
} bb_get_1v1_info_out_t;

typedef struct {
  uint32_t timestamp; 
} bb_get_ap_time_out_t;

typedef struct {
  uint8_t chan_num;  
  uint8_t auto_mode; 
  uint8_t acs_chan;  
  uint8_t
      work_chan;                         
  uint32_t freq[BB_CONFIG_MAX_CHAN_NUM]; 
  int32_t power[BB_CONFIG_MAX_CHAN_NUM]; 
} bb_get_chan_info_out_t;

typedef struct {
  uint8_t slot_bmp; 
} bb_get_distc_result_in_t;

typedef struct {
  int32_t distance
      [BB_SLOT_MAX]; 
} bb_get_distc_result_out_t;

typedef struct {
  uint8_t dir;  
  uint8_t slot; 
} bb_get_mcs_in_t;

typedef struct {
  uint8_t mcs; 
  uint32_t
      throughput; 
} bb_get_mcs_out_t;

typedef struct {
  uint16_t snr;
  uint16_t ldpc_err;
  uint16_t ldpc_num;
  uint8_t  gain_a;
  uint8_t  gain_b;
  /* The SDK header declares this struct as 8 bytes, but the daemon's real
   * wire size is 16 (independently confirmed by the ar8030-transport
   * project, whose SDK patch 0030 fixes the same thing). That is exactly the
   * 98-vs-164-byte mismatch seen on BB_GET_PAIR_RESULT: 8 slots x 8 missing
   * bytes. The extra fields' meaning is unknown; padding to the true size
   * keeps every array-of-bb_quality_t struct the length the daemon sends. */
  uint8_t  reserved[8];
} bb_quality_t;

typedef struct {
  uint8_t slot_bmp;
  bb_mac_t peer_mac[BB_SLOT_MAX];
  /* The daemon puts quality[] at offset 36, not 34. peer_mac ends at 33 and
   * bb_quality_t above is only 2-aligned, so the natural layout lands the
   * array two bytes early - every quality reading shifted - and makes the
   * struct 162 where the daemon's own length table says 164. That table is
   * embedded in ar_ldy_gnd (99 records of {cmd, in_len, out_len}) and is the
   * authority here. Pad explicitly. */
  uint8_t _pad_to_36[3];
  bb_quality_t quality[BB_SLOT_MAX];
} bb_get_pair_out_t;

typedef struct {
  uint16_t
      user_bmp; 
} bb_get_status_in_t;

typedef struct {
  uint8_t state;     
  uint8_t rx_mcs;    
  bb_mac_t peer_mac; 
} bb_link_status_t;

typedef struct {
  uint8_t mcs; 
  uint8_t rf_mode; 
  uint8_t
      tintlv_enable;  
  uint8_t tintlv_num; 
  uint8_t tintlv_len; 
  uint8_t bandwidth;  
  uint32_t freq_khz;  
} bb_phy_status_t;

typedef struct {
  bb_phy_status_t tx_status; 
  bb_phy_status_t rx_status; 
} bb_user_status_t;

typedef struct {
  uint8_t role;        
  uint8_t mode;        
  uint8_t sync_mode;   
  uint8_t sync_master; 
  uint8_t cfg_sbmp;    
  uint8_t rt_sbmp;     
  bb_mac_t mac;        
  bb_user_status_t user_status[BB_DATA_USER_MAX]; 
  bb_link_status_t link_status[BB_SLOT_MAX];      
} bb_get_status_out_t;

typedef struct bb_host_t
    bb_host_t;

typedef struct {
  bb_mac_t mac; 
} bb_set_ap_mac_t;

typedef struct {
  uint8_t slot;      
  uint8_t dir;       
  uint8_t bandwidth; 
} bb_set_bandwidth_t;

typedef struct {
  uint8_t
      auto_mode;
  /* The daemon's length table says BB_SET_CHAN_MODE takes 2 bytes, not 1, so
   * sending this struct as-is made it read a byte of our stack as the second
   * field. Which field that is has NOT been established: the sibling
   * bb_set_mcs_mode_t is {slot, auto_mode}, so the real layout may well put
   * slot first, in which case our auto_mode has been landing in slot all
   * along. Channel auto behaves correctly today, which argues for this order,
   * so the byte is added as padding rather than reordering on a guess.
   * Zeroing it at least makes the call deterministic. */
  uint8_t reserved;
} bb_set_chan_mode_t;

typedef struct {
  uint8_t chan_dir;   
  uint8_t chan_index; 
} bb_set_chan_t;

typedef struct {
  bb_event_e event;           
  bb_event_callback callback; 
  void *user;                 
} bb_set_event_callback_t;

typedef struct {
  uint8_t user;      
  uint8_t dir_bmp;   
  uint32_t freq_khz; 
} bb_set_freq_t;

typedef struct {
  uint8_t mode; 
} bb_set_lna_mode_t;

typedef struct {
  uint8_t slot; 
  uint8_t
      auto_mode; 
} bb_set_mcs_mode_t;

typedef struct {
  uint8_t slot; 
  uint8_t mcs; 
} bb_set_mcs_t;

typedef struct {
  uint8_t start;    
  uint8_t slot_bmp; 
  bb_mac_t black_list[BB_BLACK_LIST_SIZE]; 
} bb_set_pair_mode_t;

typedef struct bb_sock_opt_t {
  uint32_t
      tx_buf_size; 
  uint32_t
      rx_buf_size; 
} bb_sock_opt_t;

typedef struct {
    uint8_t chan_num;
    uint8_t chan_idx[128];
} bb_work_chan_list_t;

typedef bb_conf_candidates_t
    bb_set_candidate_t;

typedef bb_get_pwr_auto_out_t
    bb_set_pwr_auto_in_t;

typedef bb_get_cur_pwr_out_t
    bb_set_pwr_in_t;

int bb_init(bb_dev_handle_t *handle);
int bb_start(bb_dev_handle_t *handle);
int bb_dev_getlist(bb_host_t *phost, bb_dev_list_t **plist);
int bb_dev_freelist(bb_dev_list_t *plist);
int bb_dev_close(bb_dev_handle_t *handle);
int bb_host_connect(bb_host_t **phost, const char *addr, int port);
int bb_host_disconnect(bb_host_t *phost);
int bb_ioctl(bb_dev_handle_t *dev, uint32_t request, const void *in, void *out);
int bb_ioctl_ex(bb_dev_handle_t *dev, uint32_t request, const void *input, void *output, int timeout);
int bb_socket_open(bb_dev_handle_t *dev, bb_slot_e slot, uint32_t port, uint32_t flag, bb_sock_opt_t *opt);
int bb_socket_read(int sockfd, void *buf, uint32_t len, int timeout);
int bb_socket_write(int sockfd, const void *buf, uint32_t len, int timeout);
int bb_socket_close(int sockfd);

AR8030_API bb_dev_handle_t *bb_dev_open(bb_dev_t *devs);

#ifdef __cplusplus
}
#endif
#endif /* FPVOS_BB_CLIENT_H */


