#pragma once

#include "tt_metal/impl/buffers/buffer.hpp"
#include "tt_metal/impl/buffers/circular_buffer.hpp"
#include "tt_metal/impl/buffers/semaphore.hpp"
#include "tt_metal/impl/device/device.hpp"
#include "tt_metal/impl/kernels/kernel.hpp"
#include "common/tt_backend_api_types.hpp"
#include "hostdevcommon/common_values.hpp"

namespace tt {

namespace tt_metal {

struct KernelGroup {
    ComputeKernel *compute = nullptr;
    DataMovementKernel *riscv_0 = nullptr;
    DataMovementKernel *riscv_1 = nullptr;
};

typedef std::map<CoreCoord, std::map<RISCV, std::vector<u32>>> RuntimeArgs;

class Program {
   public:
    Program() {}

    Program(const Program &other) = delete;
    Program& operator=(const Program &other) = delete;

    Program(Program &&other);
    Program& operator=(Program &&other);

    ~Program();

    std::vector<Kernel *> kernels() const { return kernels_; }

    std::vector<CircularBuffer *> circular_buffers() const { return circular_buffers_; }

    std::vector<Semaphore *> semaphores() const { return semaphores_; }

    std::vector<ComputeKernel *> compute_kernels() const;

    std::vector<DataMovementKernel *> data_movement_kernels() const;

    KernelGroup kernels_on_core(const CoreCoord &core) const;

    std::map<CoreCoord, KernelGroup> core_to_kernel_group() const;

    std::vector<CircularBuffer *> circular_buffers_on_core(const CoreCoord &core) const;

    std::vector<Semaphore *> semaphores_on_core(const CoreCoord &core) const;

    std::vector<CoreCoord> logical_cores() const;

    std::vector<std::string> cores_to_ops() const;

    RuntimeArgs const &runtime_args() const { return core_to_runtime_args_; }

   private:
    std::vector<Kernel *> kernels_;
    std::vector<CircularBuffer *> circular_buffers_;
    std::vector<Semaphore *> semaphores_;
    RuntimeArgs core_to_runtime_args_;

    friend DataMovementKernel *CreateDataMovementKernel(
        Program &program,
        const std::string &file_name,
        const CoreCoord &core,
        const std::vector<uint32_t> &compile_args,
        DataMovementProcessor processor_type,
        NOC noc);

    friend DataMovementKernel *CreateDataMovementKernel(
        Program &program,
        const std::string &file_name,
        const CoreCoord &core,
        DataMovementProcessor processor_type,
        NOC noc);

    friend DataMovementKernel *CreateDataMovementKernel(
        Program &program,
        const std::string &file_name,
        const CoreRange &core_range,
        const std::vector<uint32_t> &compile_args,
        DataMovementProcessor processor_type,
        NOC noc);

    friend DataMovementKernel *CreateDataMovementKernel(
        Program &program,
        const std::string &file_name,
        const CoreRange &core_range,
        DataMovementProcessor processor_type,
        NOC noc);

    friend DataMovementKernel *CreateDataMovementKernel(
        Program &program,
        const std::string &file_name,
        const CoreRangeSet &core_range_set,
        const std::vector<uint32_t> &compile_args,
        DataMovementProcessor processor_type,
        NOC noc);

    friend DataMovementKernel *CreateDataMovementKernel(
        Program &program,
        const std::string &file_name,
        const CoreRangeSet &core_range_set,
        DataMovementProcessor processor_type,
        NOC noc);

    friend ComputeKernel *CreateComputeKernel(
        Program &program,
        const std::string &file_name,
        const CoreCoord &core,
        const std::vector<uint32_t> &compile_args,
        MathFidelity math_fidelity,
        bool fp32_dest_acc_en,
        bool math_approx_mode);

    friend ComputeKernel *CreateComputeKernel(
        Program &program,
        const std::string &file_name,
        const CoreRange &core_range,
        const std::vector<uint32_t> &compile_args,
        MathFidelity math_fidelity,
        bool fp32_dest_acc_en,
        bool math_approx_mode);

    friend ComputeKernel *CreateComputeKernel(
        Program &program,
        const std::string &file_name,
        const CoreRangeSet &core_range_set,
        const std::vector<uint32_t> &compile_args,
        MathFidelity math_fidelity,
        bool fp32_dest_acc_en,
        bool math_approx_mode);

    friend CircularBuffer *CreateCircularBuffers(
        Program &program,
        Device *device,
        uint32_t buffer_id,
        const CoreRangeSet &core_range_set,
        uint32_t num_tiles,
        uint32_t size_in_bytes,
        uint32_t l1_address,
        DataFormat data_format);

    friend CircularBuffer *CreateCircularBuffers(
        Program &program,
        Device *device,
        uint32_t buffer_index,
        const CoreRangeSet &core_range_set,
        uint32_t num_tiles,
        uint32_t size_in_bytes,
        DataFormat data_format);

    friend Semaphore *CreateSemaphore(Program &program, Device *device, const CoreRange &core_range, uint32_t initial_value);

    friend Semaphore *CreateSemaphore(Program &program, Device *device, const CoreRangeSet &core_range_set, uint32_t initial_value);

    friend void SetRuntimeArgs(Program &program, Kernel *kernel, const CoreCoord &logical_core, const std::vector<uint32_t> &runtime_args);

    void add_kernel(Kernel *kernel) { kernels_.push_back(kernel); }

    void add_circular_buffer(CircularBuffer *circular_buffer) { circular_buffers_.push_back(circular_buffer); }

    void add_semaphore(Semaphore *semaphore) { semaphores_.push_back(semaphore); }

    void set_runtime_args(const CoreCoord &logical_core, const RISCV &riscv, const std::vector<uint32_t> &runtime_args);
};

}  // namespace tt_metal

}  // namespace tt
