# 32-Bit-Hosttests (gcc -m32, ASan/UBSan) wie auf CachyOS – fuer den Mac (colima, x86_64-VM)
FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends gcc gcc-multilib libc6-dev-i386 make && rm -rf /var/lib/apt/lists/*
