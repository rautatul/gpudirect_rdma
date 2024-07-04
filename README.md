
Sample libibverbs based apps for GPUDirect RDMA 

Steps for Server Side
# git clone https://github.com/intel-innersource/networking.smartnic.rdma.test-sw
# cd ofed/read-write
# make
# ./server -q 1

Steps to test the app on Client side
# git clone https://github.com/linux-rdma/rdma-core
# Apply irfa proider patch
# Proram SOC, reboot SoC & Host, ran SoC & Host side Scripts
# git clone https://github.com/rautatul/gpudirect_rdma
# cd  gpudirect_rdma
# make
# ./client -w -q 1 -b 4096

Contact for any Queries 
Atul Raut
email - atul.raut@intel.com
