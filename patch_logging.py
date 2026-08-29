import sys

p = 'src/hle/libkernel.cpp'
d = open(p, 'r').read()
d = d.replace('LOG_DEBUG(HLE, "libkernel::fseek', 'LOG_ERROR(HLE, "libkernel::fseek')
d = d.replace('LOG_DEBUG(HLE, "libkernel::ftell', 'LOG_ERROR(HLE, "libkernel::ftell')
d = d.replace('LOG_DEBUG(HLE, "sceKernelFstat', 'LOG_ERROR(HLE, "sceKernelFstat')
d = d.replace('LOG_DEBUG(HLE, "sceKernelStat', 'LOG_ERROR(HLE, "sceKernelStat')
open(p, 'w').write(d)
