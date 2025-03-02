// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <unistd.h>
#include <cstdint>

#include "risc_common.h"
#include "tensix.h"
#include "tensix_types.h"
#include "noc.h"
#include "noc_overlay_parameters.h"
#include "ckernel_structs.h"
#include "stream_io_map.h"
#include "c_tensix_core.h"
#include "tdma_xmov.h"
#include "noc_nonblocking_api.h"
#include "firmware_common.h"
#include "tools/profiler/kernel_profiler.hpp"
#include "dev_msgs.h"
#include "risc_attribs.h"
#include "generated_bank_to_noc_coord_mapping.h"
#include "circular_buffer.h"
#include "dataflow_api.h"

#include "debug/watcher_common.h"
#include "debug/waypoint.h"
#include "debug/stack_usage.h"

uint8_t noc_index;

uint32_t noc_reads_num_issued[NUM_NOCS] __attribute__((used));
uint32_t noc_nonposted_writes_num_issued[NUM_NOCS] __attribute__((used));
uint32_t noc_nonposted_writes_acked[NUM_NOCS] __attribute__((used));
uint32_t noc_nonposted_atomics_acked[NUM_NOCS] __attribute__((used));
uint32_t noc_posted_writes_num_issued[NUM_NOCS] __attribute__((used));

uint32_t tt_l1_ptr *rta_l1_base __attribute__((used));
uint32_t tt_l1_ptr *crta_l1_base __attribute__((used));
uint32_t tt_l1_ptr *sem_l1_base[ProgrammableCoreType::COUNT] __attribute__((used));

uint8_t my_x[NUM_NOCS] __attribute__((used));
uint8_t my_y[NUM_NOCS] __attribute__((used));

//c_tensix_core core;

tt_l1_ptr mailboxes_t * const mailboxes = (tt_l1_ptr mailboxes_t *)(MEM_IERISC_MAILBOX_BASE);

CBInterface cb_interface[NUM_CIRCULAR_BUFFERS] __attribute__((used));

#if defined(PROFILE_KERNEL)
namespace kernel_profiler {
    uint32_t wIndex __attribute__((used));
    uint32_t stackSize __attribute__((used));
    uint32_t sums[SUM_COUNT] __attribute__((used));
    uint32_t sumIDs[SUM_COUNT] __attribute__((used));
}
#endif

//inline void RISC_POST_STATUS(uint32_t status) {
//  volatile uint32_t* ptr = (volatile uint32_t*)(NOC_CFG(ROUTER_CFG_2));
//  ptr[0] = status;
//}

void set_deassert_addresses() {
#ifdef ARCH_BLACKHOLE
    // start_pc1 make this a const!
    WRITE_REG(0xFFB14008, MEM_SLAVE_IERISC_FIRMWARE_BASE);
#endif
}

void init_sync_registers() {
    volatile tt_reg_ptr uint* tiles_received_ptr;
    volatile tt_reg_ptr uint* tiles_acked_ptr;
    for (uint32_t operand = 0; operand < NUM_CIRCULAR_BUFFERS; operand++) {
      tiles_received_ptr = get_cb_tiles_received_ptr(operand);
      tiles_received_ptr[0] = 0;
      tiles_acked_ptr = get_cb_tiles_acked_ptr(operand);
      tiles_acked_ptr[0] = 0;
    }
}

inline void run_slave_eriscs(dispatch_core_processor_masks enables) {
    if (enables & DISPATCH_CLASS_MASK_ETH_DM1) {
        mailboxes->slave_sync.dm1 = RUN_SYNC_MSG_GO;
    }
}

inline void wait_slave_eriscs(uint32_t &heartbeat) {
    WAYPOINT("SEW");
    while (mailboxes->slave_sync.all != RUN_SYNC_MSG_ALL_SLAVES_DONE) {
        RISC_POST_HEARTBEAT(heartbeat);
    }
    WAYPOINT("SED");
}

int main() {
    conditionally_disable_l1_cache();
    DIRTY_STACK_MEMORY();
    WAYPOINT("I");
    do_crt1((uint32_t *)MEM_IERISC_INIT_LOCAL_L1_BASE_SCRATCH);
    uint32_t heartbeat = 0;

    risc_init();

    mailboxes->slave_sync.all = RUN_SYNC_MSG_ALL_SLAVES_DONE;
    set_deassert_addresses();
    //device_setup();

    noc_init(MEM_NOC_ATOMIC_RET_VAL_ADDR);

    deassert_all_reset(); // Bring all riscs on eth cores out of reset
    mailboxes->go_message.signal = RUN_MSG_DONE;
    mailboxes->launch_msg_rd_ptr = 0; // Initialize the rdptr to 0
    // Cleanup profiler buffer incase we never get the go message


    while (1) {

        init_sync_registers();
        // Wait...
        WAYPOINT("GW");
        while (mailboxes->go_message.signal != RUN_MSG_GO)
        {
            RISC_POST_HEARTBEAT(heartbeat);
        };
        WAYPOINT("GD");

        {
            // Idle ERISC Kernels aren't given go-signals corresponding to empty launch messages. Always profile this iteration, since it's guaranteed to be valid.
            DeviceZoneScopedMainN("ERISC-IDLE-FW");
            uint32_t launch_msg_rd_ptr = mailboxes->launch_msg_rd_ptr;
            launch_msg_t* launch_msg_address = &(mailboxes->launch[launch_msg_rd_ptr]);
            DeviceZoneSetCounter(launch_msg_address->kernel_config.host_assigned_id);

            noc_index = launch_msg_address->kernel_config.brisc_noc_id;

            flush_erisc_icache();

            enum dispatch_core_processor_masks enables = (enum dispatch_core_processor_masks)launch_msg_address->kernel_config.enables;
            run_slave_eriscs(enables);

            uint32_t kernel_config_base = firmware_config_init(mailboxes, ProgrammableCoreType::IDLE_ETH, DISPATCH_CLASS_ETH_DM0);
            uint32_t tt_l1_ptr *cb_l1_base = (uint32_t tt_l1_ptr *)(kernel_config_base +
                launch_msg_address->kernel_config.cb_offset);

            // Run the ERISC kernel
            if (enables & DISPATCH_CLASS_MASK_ETH_DM0) {
                WAYPOINT("R");
                int index = static_cast<std::underlying_type<EthProcessorTypes>::type>(EthProcessorTypes::DM0);
                void (*kernel_address)(uint32_t) = (void (*)(uint32_t))
                    (kernel_config_base + mailboxes->launch[mailboxes->launch_msg_rd_ptr].kernel_config.kernel_text_offset[index]);
                (*kernel_address)((uint32_t)kernel_address);
                RECORD_STACK_USAGE();
                WAYPOINT("D");
            }

            wait_slave_eriscs(heartbeat);

            mailboxes->go_message.signal = RUN_MSG_DONE;

            // Notify dispatcher core that it has completed
            if (launch_msg_address->kernel_config.mode == DISPATCH_MODE_DEV) {
                launch_msg_address->kernel_config.enables = 0;
                uint64_t dispatch_addr =
                    NOC_XY_ADDR(NOC_X(mailboxes->go_message.master_x),
                        NOC_Y(mailboxes->go_message.master_x), DISPATCH_MESSAGE_ADDR + mailboxes->go_message.dispatch_message_offset);
                DEBUG_SANITIZE_NOC_ADDR(noc_index, dispatch_addr, 4);
                CLEAR_PREVIOUS_LAUNCH_MESSAGE_ENTRY_FOR_WATCHER();
                noc_fast_atomic_increment(noc_index, NCRISC_AT_CMD_BUF, dispatch_addr, NOC_UNICAST_WRITE_VC, 1, 31 /*wrap*/, false /*linked*/);
                mailboxes->launch_msg_rd_ptr = (launch_msg_rd_ptr + 1) & (launch_msg_buffer_num_entries - 1);
            }

#ifndef ARCH_BLACKHOLE
            while (1) {
                RISC_POST_HEARTBEAT(heartbeat);
            }
#endif
        }
    }

    return 0;
}
