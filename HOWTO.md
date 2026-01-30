# Creating python virtual environment
python3 -m venv venv/e2sar

# activate venv
. venv/e2sar/bin/activate

# Install the latest version of meson
pip install meson

# Build the project
more README.md 
meson setup build/
meson comile -C build/

# Run the receiver
cd build/src
./e2sar-root -r -u 'ejfats://b50ce5e796d6460eb0a4de4a82621314@ejfat-lb.es.net:18008/lb/271?sync=192.188.29.6:19010&data=192.188.29.10&data=[2001:400:a300::10]' --withcp --recv-ip 129.57.177.8 --recv-port 10000 --recv-threads 8

# Run the sender
cd build/src
./e2sar-root -s -u 'ejfats://b50ce5e796d6460eb0a4de4a82621314@ejfat-lb.es.net:18008/lb/271?sync=192.188.29.6:19010&data=192.188.29.10&data=[2001:400:a300::10]' --withcp --files /nvme/haidis/toy_data/dalitz_toy_data_0/dalitz_root_file_0.root --tree dalitz_root_tree --bufsize-mb 1 --mtu 9000


