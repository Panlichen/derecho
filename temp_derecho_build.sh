if [ -d "build" ]; then
  rm -rf build
fi
INSTALL_PREFIX="/usr/local"

mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} ..
make -j `lscpu | grep "^CPU(" | awk '{print $2}'`
sudo make install
