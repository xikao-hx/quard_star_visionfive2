#!/bin/bash
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
top_dir=$(cd "$script_dir/.." && pwd)
cd "$top_dir"

if [ ! -f /opt/riscv/bin/riscv64-unknown-elf-gcc ]; then
    echo "Try to install riscv64-unknown-elf-gcc toolchain ..."

    ## get toolchain url according ubuntu version
    ubuntu_major_version=$(lsb_release -rs | cut -d. -f1)
    echo "Ubuntu major version: $ubuntu_major_version"
    if [ $ubuntu_major_version = "18" ]; then
        download_url="https://github.com/riscv-collab/riscv-gnu-toolchain/releases/download/2022.04.12/riscv64-elf-ubuntu-18.04-nightly-2022.04.12-nightly.tar.gz"
    elif [ $ubuntu_major_version = "20" ]; then
        download_url="https://github.com/riscv-collab/riscv-gnu-toolchain/releases/download/2022.04.12/riscv64-elf-ubuntu-20.04-nightly-2022.04.12-nightly.tar.gz"
    elif [ $ubuntu_major_version = "22" ]; then
        download_url="https://github.com/riscv-collab/riscv-gnu-toolchain/releases/download/2022.11.16/riscv64-elf-ubuntu-22.04-nightly-2022.11.16-nightly.tar.gz"
    else
        echo "no toolchain support ubuntu $(lsb_release -rs) !!"
        exit 1
    fi

    ## Download and install toolchain according ubuntu version
    filename=$(basename $download_url)
    download_status="failed"
    retry_count=3
    while [ $retry_count -gt 0 ] && [ "$download_status" = "failed" ]; do
        wget $download_url -O $filename
        if [ $? -eq 0 ]; then
            echo "Download succeeded."
            download_status="successed"
        else
            echo "Download failed. Retrying..."
            retry_count=$((retry_count-1))
        fi
    done
    if [ $download_status = "failed" ] || [ ! -f $filename ]; then
        echo "Download toolchain $download_url still failed, please check your network"
        exit 1
    fi

    ## untar and install to /opt, Note this will ask sudo
    sudo tar xf $filename -C /opt
    if [ ! $? -eq 0 ]; then
        echo "toolchain $filename broken, please try agin"
        exit 1
    fi
    if [ -d /opt/riscv ] && [ -f /opt/riscv/bin/riscv64-unknown-elf-gcc ]; then
        /opt/riscv/bin/riscv64-unknown-elf-gcc --version
        echo "riscv64-unknown-elf-gcc toolchain had been successed installed"
    fi
    if [ -f $filename ]; then
        rm -rf $filename
    fi
fi

# Build the 6.6 AMP image. Select the runtime with AMP_RTOS=freertos or
# AMP_RTOS=rtthread; FreeRTOS is the default.
echo "Build the whole ${AMP_RTOS:-freertos} AMP FIT image and sdcard img ..."
make -j$(nproc)
make buildroot_rootfs -j$(nproc)
make ampuboot_fit AMP_RTOS=${AMP_RTOS:-freertos} -j$(nproc)
make img
make amp_img
