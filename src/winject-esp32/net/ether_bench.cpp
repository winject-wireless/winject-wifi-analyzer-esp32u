#include "netstack.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint16_t CONTROL=5500, DATA=5501;
constexpr size_t LEN=1400;
netstack* netp;
std::atomic<uint32_t> rx_packets{0}, rx_bytes{0}, tx_attempted{0}, tx_ok{0}, tx_fail{0};
std::atomic<bool> running{false};
struct params { netstack::udp_peer peer; unsigned rate; unsigned ms; } job;
void sender(void*) {
    uint8_t frame[net::k_eth_max] = {};
    uint8_t* p=frame+net::k_udp_payload_off;
    memcpy(p,"ETST",4);
    const int64_t start=esp_timer_get_time(), end=start+int64_t(job.ms)*1000;
    uint32_t seq=0;
    while (esp_timer_get_time()<end) {
        int64_t due=start+(int64_t(seq)*LEN*8*1000)/job.rate;
        int64_t now=esp_timer_get_time();
        if (now<due) { if (due-now>2000) vTaskDelay(1); continue; }
        net::wr32(p+4,seq++); tx_attempted.fetch_add(1);
        if(netp->send_udp(frame,LEN,job.peer,DATA)) tx_ok.fetch_add(1); else tx_fail.fetch_add(1);
        if ((seq & 63)==0) vTaskDelay(1);
    }
    running.store(false); vTaskDelete(nullptr);
}
void data_rx(void*, const netstack::udp_rx& r) {
    if(r.len>=8 && memcmp(r.data,"ETST",4)==0) { rx_packets.fetch_add(1); rx_bytes.fetch_add(r.len); }
}
void control_rx(void*, const netstack::udp_rx& r) {
    char cmd[100]={}; size_t n=r.len<sizeof(cmd)-1?r.len:sizeof(cmd)-1; memcpy(cmd,r.data,n);
    char reply[256]; unsigned rate=0, ms=0;
    if (!strcmp(cmd,"RESET") && !running.load()) {
        rx_packets=0; rx_bytes=0; tx_attempted=0; tx_ok=0; tx_fail=0;
        snprintf(reply,sizeof(reply),"OK RESET");
    } else if (sscanf(cmd,"TX %u %u",&rate,&ms)==2 && rate>=1 && rate<=100000 && ms>=100 && ms<=60000 && !running.load()) {
        job.peer={r.src_ip,DATA,r.src_mac}; job.rate=rate; job.ms=ms;
        running.store(true);
        if(xTaskCreatePinnedToCore(sender,"eth_bench_tx",4096,nullptr,4,nullptr,1)!=pdPASS) { running=false; snprintf(reply,sizeof(reply),"ERR task"); }
        else snprintf(reply,sizeof(reply),"OK TX %u %u",rate,ms);
    } else if (!strcmp(cmd,"STAT")) {
        auto s=netp->stats();
        snprintf(reply,sizeof(reply),"STAT rx_packets=%lu rx_bytes=%lu tx_attempted=%lu tx_ok=%lu tx_fail=%lu running=%u net_rx_drop=%lu net_tx_err=%lu",(unsigned long)rx_packets.load(),(unsigned long)rx_bytes.load(),(unsigned long)tx_attempted.load(),(unsigned long)tx_ok.load(),(unsigned long)tx_fail.load(),running.load(),(unsigned long)s.rx_dropped,(unsigned long)s.tx_errors);
    } else snprintf(reply,sizeof(reply),"ERR command_or_busy");
    uint8_t frame[net::k_eth_max]; memcpy(frame+net::k_udp_payload_off,reply,strlen(reply));
    netstack::udp_peer peer={r.src_ip,r.src_port,r.src_mac}; netp->send_udp(frame,strlen(reply),peer,CONTROL);
}
}
void ethernet_bench_init(netstack& n) {
    netp=&n; n.set_udp_handler(CONTROL,control_rx,nullptr); n.set_udp_handler(DATA,data_rx,nullptr);
    ESP_LOGI("eth_bench","UDP control %u data %u",CONTROL,DATA);
}

