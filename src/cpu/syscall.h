#pragma once
//
// NOTE: There is no CPU-side syscall table. The single syscall dispatch
// table lives in src/kernel/syscalls.cpp (Kernel::InitializeSyscallTable /
// Kernel::HandleSyscall / Kernel::RegisterSyscallHandler); CpuCore's syscall
// entry points in src/cpu/cpu.cpp are thin adapters over it.
//
