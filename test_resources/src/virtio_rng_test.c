#include "io.h"
#include "log.h"
#include "virtio/virtio_mmio.h"
#include "virtio/virtio_queue.h"

#include <stdint.h>

#define QUEUE_SIZE 8
#define ENTROPY_LEN 32
#define AVAIL_RING_SIZE (sizeof(uint16_t) * QUEUE_SIZE + sizeof(VirtQueueAvail))
#define USED_RING_SIZE (sizeof(VirtQueueUsedElem) * QUEUE_SIZE + sizeof(VirtQueueUsed))
#define VIRTIO_MMIO_BASE 0x10001000ull
#define VIRTIO_MAGIC 0x74726976u
#define VIRTIO_VERSION 2u
#define VIRTIO_ID_RNG 4u

struct VirtIOMMIOLayout {
    uint32_t magic_value;
    uint32_t version;
    uint32_t device_id;
    uint32_t vendor_id;
    uint32_t device_features;
    uint32_t device_features_sel;
    uint32_t reserved_0[2];
    uint32_t driver_features;
    uint32_t driver_features_sel;
    uint32_t reserved_1[2];
    uint32_t queue_sel;
    uint32_t queue_num_max;
    uint32_t queue_num;
    uint32_t queue_align;
    uint32_t queue_pfn;
    uint32_t queue_ready;
    uint32_t reserved_2[2];
    uint32_t queue_notify;
    uint32_t reserved_3[3];
    uint32_t interrupt_status;
    uint32_t interrupt_ack;
    uint32_t reserved_4[2];
    uint32_t status;
    uint32_t reserved_5[3];
    uint32_t queue_desc_low;
    uint32_t queue_desc_high;
    uint32_t reserved_6[2];
    uint32_t queue_avail_low;
    uint32_t queue_avail_high;
    uint32_t reserved_7[2];
    uint32_t queue_used_low;
    uint32_t queue_used_high;
};

static volatile struct VirtIOMMIOLayout* rng =
    (volatile struct VirtIOMMIOLayout*)VIRTIO_MMIO_BASE;

static VirtQueueDesc desc[QUEUE_SIZE] __attribute__((aligned(16)));
static uint8_t avail_base[AVAIL_RING_SIZE] __attribute__((aligned(8)));
static uint8_t used_base[USED_RING_SIZE] __attribute__((aligned(8)));
static uint8_t entropy[ENTROPY_LEN] __attribute__((aligned(8)));
static uint8_t previous[ENTROPY_LEN];

static VirtQueueAvail* avail = (VirtQueueAvail*)avail_base;
static VirtQueueUsed* used = (VirtQueueUsed*)used_base;

static int wait_interrupt(void) {
    for (int i = 0; i < 1000000; i++) {
        if (rng->interrupt_status & VIRTIO_MMIO_INT_VRING) {
            return 1;
        }
    }
    return 0;
}

static void request_entropy(uint16_t desc_index, uint16_t avail_index) {
    for (int i = 0; i < ENTROPY_LEN; i++) {
        entropy[i] = 0xA5;
    }
    desc[desc_index].paddr = (uint64_t)entropy;
    desc[desc_index].len = ENTROPY_LEN;
    desc[desc_index].flags = VIRTQ_DESC_F_WRITE;
    desc[desc_index].next = 0;
    avail->ring[avail_index] = desc_index;
    avail->idx = (uint16_t)(avail_index + 1);
    rng->queue_notify = 0;
}

static int entropy_is_fresh(void) {
    int changed = 0;
    int distinct = 0;
    for (int i = 0; i < ENTROPY_LEN; i++) {
        if (entropy[i] != 0xA5) {
            changed = 1;
        }
        if (entropy[i] != entropy[0]) {
            distinct = 1;
        }
    }
    return changed && distinct;
}

int main(void) {
    TEST_START(__func__);

    if (rng->magic_value != VIRTIO_MAGIC || rng->version != VIRTIO_VERSION ||
        rng->device_id != VIRTIO_ID_RNG) {
        Log(ERROR, "virtio-rng not found at 0x10001000 (magic=%x version=%x id=%x)\n",
            rng->magic_value, rng->version, rng->device_id);
        fail();
    }

    rng->status = ACKNOWLEDGE;
    rng->status = DRIVER;
    rng->device_features_sel = 1;
    uint32_t feature_word = rng->device_features;
    if ((feature_word & 1u) == 0) {
        Log(ERROR, "device does not offer VIRTIO_F_VERSION_1\n");
        fail();
    }
    rng->driver_features_sel = 1;
    rng->driver_features = 1;
    rng->status = FEATURES_OK;
    if ((rng->status & FEATURES_OK) == 0) {
        Log(ERROR, "feature negotiation failed\n");
        fail();
    }

    rng->queue_sel = 0;
    if (rng->queue_num_max < QUEUE_SIZE) {
        Log(ERROR, "queue 0 is too small\n");
        fail();
    }
    rng->queue_num = QUEUE_SIZE;
    rng->queue_desc_low = (uint32_t)(uint64_t)desc;
    rng->queue_desc_high = (uint32_t)(((uint64_t)desc) >> 32);
    rng->queue_avail_low = (uint32_t)(uint64_t)avail;
    rng->queue_avail_high = (uint32_t)(((uint64_t)avail) >> 32);
    rng->queue_used_low = (uint32_t)(uint64_t)used;
    rng->queue_used_high = (uint32_t)(((uint64_t)used) >> 32);
    rng->queue_ready = 1;
    rng->status = DRIVER_OK;

    avail->flags = AvailFlagDefault;
    avail->idx = 0;
    used->flags = UsedFlagDefault;
    used->idx = 0;

    request_entropy(0, 0);
    if (!wait_interrupt()) {
        Log(ERROR, "first entropy request was not completed\n");
        fail();
    }
    rng->interrupt_ack = VIRTIO_MMIO_INT_VRING;
    if (used->ring[0].id != 0 || used->ring[0].len != ENTROPY_LEN || !entropy_is_fresh()) {
        Log(ERROR, "first entropy buffer is invalid, len=%d\n", used->ring[0].len);
        fail();
    }
    for (int i = 0; i < ENTROPY_LEN; i++) {
        previous[i] = entropy[i];
    }

    request_entropy(1, 1);
    if (!wait_interrupt()) {
        Log(ERROR, "second entropy request was not completed\n");
        fail();
    }
    rng->interrupt_ack = VIRTIO_MMIO_INT_VRING;
    if (used->ring[1].id != 1 || used->ring[1].len != ENTROPY_LEN || !entropy_is_fresh()) {
        Log(ERROR, "second entropy buffer is invalid, len=%d\n", used->ring[1].len);
        fail();
    }

    int same = 1;
    for (int i = 0; i < ENTROPY_LEN; i++) {
        if (entropy[i] != previous[i]) {
            same = 0;
        }
    }
    if (same) {
        Log(ERROR, "two entropy reads returned the same bytes\n");
        fail();
    }

    printf("virtio-rng sample:");
    for (int i = 0; i < 8; i++) {
        printf(" %x", entropy[i]);
    }
    printf("\n");
    pass();
    return 0;
}
