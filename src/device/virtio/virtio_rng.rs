use core::slice;
use std::sync::{
    Arc,
    atomic::{AtomicU8, Ordering},
};

use log::info;
use rand::{TryRngCore, rngs::OsRng};
use tokio::sync::{mpsc, watch};

use crate::{
    device::virtio::{
        config::{VirtIOFeatureSet, virtio_reserved_feature},
        virtio_device::{VirtIODeviceEnum, VirtIODeviceTrait},
        virtio_mmio::VirtIODeviceStatus,
        virtio_queue::{VirtQueue, VirtQueueCompletion, VirtQueueDesc},
    },
    ram_config,
    task_spawner::{DeviceTask, TaskSpawner},
};

const VIRTIO_RNG_TASK_CAPACITY: usize = 64;
pub(crate) const VIRTIO_DEVICE_ID_RNG: u16 = VirtIODeviceEnum::VirtIORng as u16;

type EntropyFill = fn(&mut [u8]);

fn fill_os_entropy(buf: &mut [u8]) {
    OsRng
        .try_fill_bytes(buf)
        .expect("virtio-rng failed to read OS entropy");
}

struct RngBuffer {
    addr: usize,
    len: usize,
}

struct RngCompletion {
    written: u32,
    queue_completion: VirtQueueCompletion,
    isr: Arc<AtomicU8>,
}

impl RngCompletion {
    fn complete(self) {
        // Used-ring publication is Release. The ISR store stays Release as well,
        // so a guest that observes the interrupt also observes the entropy bytes.
        self.queue_completion.complete(self.written);
        self.isr.fetch_or(1, Ordering::Release);
    }
}

struct RngFillTask {
    buffers: Vec<RngBuffer>,
    completion: RngCompletion,
}

enum RngTask {
    Fill(RngFillTask),
    DrainBarrier(std::sync::mpsc::SyncSender<()>),
}

struct VirtIORngAsyncWorker {
    receiver: mpsc::Receiver<RngTask>,
    fill: EntropyFill,
}

impl DeviceTask for VirtIORngAsyncWorker {
    async fn run(mut self, mut cancel: watch::Receiver<bool>) {
        loop {
            let task = tokio::select! {
                _ = cancel.changed() => return,
                task = self.receiver.recv() => match task {
                    Some(task) => task,
                    None => return,
                },
            };
            self.run_task(task).await;
            if *cancel.borrow() {
                return;
            }
        }
    }
}

impl VirtIORngAsyncWorker {
    async fn run_task(&mut self, task: RngTask) {
        std::sync::atomic::fence(Ordering::Acquire);
        match task {
            RngTask::Fill(task) => {
                for buffer in &task.buffers {
                    let data =
                        unsafe { slice::from_raw_parts_mut(buffer.addr as *mut u8, buffer.len) };
                    (self.fill)(data);
                }
                task.completion.complete();
            }
            RngTask::DrainBarrier(ack) => {
                let _ = ack.send(());
            }
        }
    }
}

pub(crate) struct VirtIORngDevice {
    status: u8,
    isr: Arc<AtomicU8>,
    host_feature: VirtIOFeatureSet,
    guest_feature: VirtIOFeatureSet,
    generation: u32,
    ram_base_raw: usize,
    sender: mpsc::Sender<RngTask>,
    queue: VirtQueue,
}

impl VirtIORngDevice {
    fn new(ram_base_raw: *mut u8, sender: mpsc::Sender<RngTask>) -> Self {
        info!("build virtio rng device.");
        Self {
            status: 0,
            isr: Arc::new(AtomicU8::new(0)),
            host_feature: virtio_reserved_feature::VERSION_1,
            guest_feature: 0,
            generation: 0,
            ram_base_raw: ram_base_raw as usize,
            sender,
            queue: VirtQueue::new(ram_base_raw, 0),
        }
    }

    fn writable_buffer(ram_base_raw: usize, desc: &VirtQueueDesc) -> Option<(usize, u32)> {
        if !desc.is_device_write() || desc.len == 0 {
            return None;
        }
        let offset = desc.paddr.checked_sub(ram_config::BASE_ADDR as u64)?;
        let end = offset.checked_add(u64::from(desc.len))?;
        if end > ram_config::SIZE as u64 {
            return None;
        }
        Some((
            desc.get_request_package::<u8>(ram_base_raw) as usize,
            desc.len,
        ))
    }
}

impl VirtIODeviceTrait for VirtIORngDevice {
    fn get_device_id(&self) -> u16 {
        VIRTIO_DEVICE_ID_RNG
    }

    fn status(&mut self) -> &mut u8 {
        &mut self.status
    }

    fn get_generation(&self) -> u32 {
        self.generation
    }

    fn reset(&mut self) {
        info!("reset virtio rng device.");
        let (drain_tx, drain_rx) = std::sync::mpsc::sync_channel(0);
        if self
            .sender
            .blocking_send(RngTask::DrainBarrier(drain_tx))
            .is_ok()
        {
            let _ = drain_rx.recv();
        }
        self.status = 0;
        self.guest_feature = 0;
        self.queue.reset();
        self.isr.store(0, Ordering::Release);
    }

    fn isr(&self) -> &AtomicU8 {
        &self.isr
    }

    fn irq_level(&mut self) -> bool {
        self.isr.load(Ordering::Acquire) != 0 && self.queue.interrupts_enabled()
    }

    fn get_host_feature(&self) -> VirtIOFeatureSet {
        self.host_feature
    }

    fn set_feature(&mut self, feature: VirtIOFeatureSet) {
        if feature & virtio_reserved_feature::VERSION_1 == 0
            || self.host_feature & feature != feature
        {
            self.status &= !VirtIODeviceStatus::FEATURES_OK.bits();
        } else {
            self.guest_feature = feature;
        }
    }

    fn set_queue_num(&mut self, num: u32) {
        self.queue.set_queue_num(num);
    }

    fn queue_select(&self, _idx: u32) {}

    fn set_desc(&mut self, addr: u64) {
        self.queue.set_desc(addr);
    }

    fn set_avail(&mut self, addr: u64) {
        self.queue.set_avail(addr);
    }

    fn set_used(&mut self, addr: u64) {
        self.queue.set_used(addr);
    }

    fn manage_one_request(&mut self) -> bool {
        let Ok(permit) = self.sender.try_reserve() else {
            return false;
        };

        let mut buffers = Vec::new();
        let mut written = 0u32;
        let ram_base_raw = self.ram_base_raw;
        let Some(queue_completion) = self.queue.take_one_request(|desc, _| {
            let Some((addr, len)) = Self::writable_buffer(ram_base_raw, desc) else {
                return;
            };
            let Some(next) = written.checked_add(len) else {
                return;
            };
            buffers.push(RngBuffer {
                addr,
                len: len as usize,
            });
            written = next;
        }) else {
            return false;
        };

        permit.send(RngTask::Fill(RngFillTask {
            buffers,
            completion: RngCompletion {
                written,
                queue_completion,
                isr: self.isr.clone(),
            },
        }));
        true
    }

    fn notify(&mut self, _idx: u32) {
        while self.manage_one_request() {}
    }

    fn queue_ready(&self) -> bool {
        self.queue.ready()
    }

    fn get_num_of_queue(&self) -> u32 {
        1
    }

    fn read_config(&mut self, _offset: u64, _len: u32) -> u64 {
        0
    }

    fn write_config(&mut self, _offset: u64, _len: u32, _data: u64) {}
}

pub(crate) struct VirtIORngDeviceBuilder {
    device: VirtIORngDevice,
    receiver: mpsc::Receiver<RngTask>,
    fill: EntropyFill,
}

impl VirtIORngDeviceBuilder {
    pub(crate) fn new(ram_base_raw: *mut u8) -> Self {
        Self::with_entropy(ram_base_raw, fill_os_entropy)
    }

    pub(crate) fn with_entropy(ram_base_raw: *mut u8, fill: EntropyFill) -> Self {
        let (sender, receiver) = mpsc::channel(VIRTIO_RNG_TASK_CAPACITY);
        Self {
            device: VirtIORngDevice::new(ram_base_raw, sender),
            receiver,
            fill,
        }
    }

    pub(crate) fn get_and_spawner_task(self, task_spawner: &mut TaskSpawner) -> VirtIORngDevice {
        let Self {
            device,
            receiver,
            fill,
        } = self;
        task_spawner.register(VirtIORngAsyncWorker { receiver, fill });
        device
    }
}

#[cfg(test)]
mod test {
    use std::{
        cell::UnsafeCell,
        thread,
        time::{Duration, Instant},
    };

    use super::*;
    use crate::{
        device::{
            DeviceTrait, PlicDevice,
            virtio::{
                config::{VIRT_MAGIC, VIRT_VERSION},
                virtio_mmio::{VirtIO_MMIO_Offset, VirtIODeviceStatus, VirtIOMMIO},
                virtio_queue::{
                    VirtQueueAvail, VirtQueueAvailFlag, VirtQueueDesc, VirtQueueDescFlag,
                    VirtQueueUsed, VirtQueueUsedFlag,
                },
            },
        },
        ram::Ram,
    };

    const QUEUE_NUM: usize = 8;
    const ENTROPY_LEN: usize = 16;

    fn test_pattern(buf: &mut [u8]) {
        for (index, byte) in buf.iter_mut().enumerate() {
            *byte = 0x11u8.wrapping_add(index as u8);
        }
    }

    fn read_u32(device: &mut VirtIOMMIO, offset: VirtIO_MMIO_Offset) -> u32 {
        device.read_u32(offset as u64).unwrap()
    }

    fn write_u32(device: &mut VirtIOMMIO, offset: VirtIO_MMIO_Offset, value: u32) {
        device.write_u32(offset as u64, value).unwrap();
    }

    fn wait_used(used: &VirtQueueUsed, expected: u16) {
        let deadline = Instant::now() + Duration::from_secs(2);
        while used.get_index() != expected {
            assert!(
                Instant::now() < deadline,
                "virtio rng worker timed out waiting for used index {expected}"
            );
            thread::yield_now();
        }
    }

    #[test]
    fn serves_entropy_and_leaves_readonly_buffers_untouched() {
        let mut ram = Ram::new();
        let ram_base = &mut ram[0] as *mut u8;
        let mut spawner = TaskSpawner::new();
        let device = VirtIORngDeviceBuilder::with_entropy(ram_base, test_pattern)
            .get_and_spawner_task(&mut spawner);
        let _worker = spawner.start();
        let mut mmio = VirtIOMMIO::new(Box::new(UnsafeCell::new(device)));

        assert_eq!(
            read_u32(&mut mmio, VirtIO_MMIO_Offset::MagicValue),
            VIRT_MAGIC
        );
        assert_eq!(
            read_u32(&mut mmio, VirtIO_MMIO_Offset::Version),
            VIRT_VERSION
        );
        assert_eq!(
            read_u32(&mut mmio, VirtIO_MMIO_Offset::DeviceId),
            VIRTIO_DEVICE_ID_RNG as u32
        );

        write_u32(
            &mut mmio,
            VirtIO_MMIO_Offset::Status,
            VirtIODeviceStatus::ACKNOWLEDGE.bits() as u32,
        );
        write_u32(
            &mut mmio,
            VirtIO_MMIO_Offset::Status,
            VirtIODeviceStatus::DRIVER.bits() as u32,
        );
        write_u32(&mut mmio, VirtIO_MMIO_Offset::DeviceFeaturesSelect, 1);
        assert_eq!(
            read_u32(&mut mmio, VirtIO_MMIO_Offset::DeviceFeatures),
            1,
            "VIRTIO_F_VERSION_1 lives in feature word 1"
        );
        write_u32(&mut mmio, VirtIO_MMIO_Offset::DriverFeaturesSelect, 1);
        write_u32(&mut mmio, VirtIO_MMIO_Offset::DriverFeatures, 1);
        write_u32(
            &mut mmio,
            VirtIO_MMIO_Offset::Status,
            VirtIODeviceStatus::FEATURES_OK.bits() as u32,
        );
        assert_ne!(
            read_u32(&mut mmio, VirtIO_MMIO_Offset::Status)
                & VirtIODeviceStatus::FEATURES_OK.bits() as u32,
            0
        );
        write_u32(
            &mut mmio,
            VirtIO_MMIO_Offset::Status,
            VirtIODeviceStatus::DRIVER_OK.bits() as u32,
        );

        write_u32(&mut mmio, VirtIO_MMIO_Offset::QueueSelect, 0);
        let queue_num_max = read_u32(&mut mmio, VirtIO_MMIO_Offset::QueueNumMax);
        assert!(queue_num_max >= QUEUE_NUM as u32);
        write_u32(&mut mmio, VirtIO_MMIO_Offset::QueueNum, QUEUE_NUM as u32);

        let desc_base = 0x8000_2000u64;
        let avail_base = 0x8000_2100 + ((QUEUE_NUM + 2) * size_of::<u16>()) as u64;
        let used_base = 0x8000_2200 + (QUEUE_NUM * size_of::<VirtQueueUsed>() + 4) as u64;
        mmio.init_queue(desc_base, avail_base, used_base);

        let descs = unsafe {
            slice::from_raw_parts_mut(
                &mut ram[(desc_base - ram_config::BASE_ADDR) as usize] as *mut u8
                    as *mut VirtQueueDesc,
                QUEUE_NUM,
            )
        };
        let avail = unsafe {
            (&mut ram[(avail_base - ram_config::BASE_ADDR) as usize] as *mut u8
                as *mut VirtQueueAvail)
                .as_mut()
                .unwrap()
        };
        avail.init(VirtQueueAvailFlag::Default);
        let avail_ring = VirtQueueAvail::mut_ring(avail as *mut _ as u64, QUEUE_NUM as u32);
        let used = unsafe {
            (&mut ram[(used_base - ram_config::BASE_ADDR) as usize] as *mut u8
                as *mut VirtQueueUsed)
                .as_mut()
                .unwrap()
        };
        used.init(VirtQueueUsedFlag::Default);

        let entropy_addr = 0x8000_3000u64;
        let entropy = unsafe {
            slice::from_raw_parts_mut(
                &mut ram[(entropy_addr - ram_config::BASE_ADDR) as usize] as *mut u8,
                ENTROPY_LEN,
            )
        };
        entropy.fill(0xA5);
        descs[0].init(
            entropy_addr,
            ENTROPY_LEN as u32,
            VirtQueueDescFlag::VIRTQ_DESC_F_WRITE,
            0,
        );
        avail_ring[0] = 0;
        avail.idx_atomic_add(1);
        write_u32(&mut mmio, VirtIO_MMIO_Offset::QueueNotify, 0);
        wait_used(used, 1);

        let expected: Vec<u8> = (0..ENTROPY_LEN)
            .map(|i| 0x11u8.wrapping_add(i as u8))
            .collect();
        assert_eq!(entropy, expected.as_slice());
        assert_eq!(used.ring(QUEUE_NUM as u32)[0].get_len(), ENTROPY_LEN as u32);
        assert_eq!(used.ring(QUEUE_NUM as u32)[0].get_id(), 0);
        assert_eq!(
            read_u32(&mut mmio, VirtIO_MMIO_Offset::InterruptStatus) & 1,
            1
        );
        assert!(mmio.irq_level());
        write_u32(&mut mmio, VirtIO_MMIO_Offset::InterruptAck, 1);
        assert!(!mmio.irq_level());

        let readonly_addr = 0x8000_3100u64;
        let readonly = unsafe {
            slice::from_raw_parts_mut(
                &mut ram[(readonly_addr - ram_config::BASE_ADDR) as usize] as *mut u8,
                ENTROPY_LEN,
            )
        };
        readonly.fill(0xAB);
        descs[1].init(
            readonly_addr,
            ENTROPY_LEN as u32,
            VirtQueueDescFlag::empty(),
            0,
        );
        avail_ring[1] = 1;
        avail.idx_atomic_add(1);
        write_u32(&mut mmio, VirtIO_MMIO_Offset::QueueNotify, 0);
        wait_used(used, 2);
        assert_eq!(readonly, &[0xABu8; ENTROPY_LEN]);
        assert_eq!(used.ring(QUEUE_NUM as u32)[1].get_len(), 0);

        write_u32(&mut mmio, VirtIO_MMIO_Offset::Status, 0);
        assert_eq!(read_u32(&mut mmio, VirtIO_MMIO_Offset::InterruptStatus), 0);
        assert_eq!(read_u32(&mut mmio, VirtIO_MMIO_Offset::Status), 0);
    }
}
