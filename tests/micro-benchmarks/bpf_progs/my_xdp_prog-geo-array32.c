#include <uapi/linux/bpf.h>
#include <uapi/linux/filter.h>
#include <uapi/linux/icmp.h>
#include <uapi/linux/if_arp.h>
#include <uapi/linux/if_ether.h>
#include <uapi/linux/if_packet.h>
#include <uapi/linux/in.h>
#include <uapi/linux/ip.h>
#include <uapi/linux/pkt_cls.h>
#include <uapi/linux/tcp.h>
#include <uapi/linux/udp.h>

struct pkt_md {
  uint32_t cnt;
  uint32_t geo_sampling_idx;
  uint64_t drop_cnt;
} __attribute__((packed));

struct geo_value {
    uint32_t geo_array[32768];
};

BPF_PERCPU_ARRAY(metadata, struct pkt_md, 1);
BPF_ARRAY(geo_sampling, struct geo_value, 1);

int xdp_prog1(struct CTXTYPE *ctx) {
    void* data_end = (void*)(long)ctx->data_end;
    void* data = (void*)(long)ctx->data;

    uint32_t zero = 0;
    struct pkt_md *md;
    
    md = metadata.lookup(&zero);
    if (!md) {
        bpf_trace_printk("Error! Invalid metadata.");
        goto DROP_ONLY;
    }

    uint32_t geo_sampl_idx = md->geo_sampling_idx;

    zero = 0;
    struct geo_value *val = geo_sampling.lookup(&zero);

    if (val) {
        geo_sampl_idx = (geo_sampl_idx + 1) & (32768 - 1);
        md->geo_sampling_idx = geo_sampl_idx;
        if (val->geo_array[geo_sampl_idx] == 123) {
            goto DROP;
        }
        // md->cnt = val->geo_array[geo_sampl_idx];
    }

DROP:;
    __sync_fetch_and_add(&md->drop_cnt, 1);

DROP_ONLY:;
    return XDP_DROP;
}