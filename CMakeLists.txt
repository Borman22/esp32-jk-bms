cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Библиотека WireGuard по умолчанию держит 1 разрешённый адрес на пир, и его занимает адрес
# самого устройства; для подсети туннеля (wg_tunnel.c: allow_tunnel_subnet) нужен второй слот; с запасом взято 4.
# Макрос меняет размер структуры, поэтому задаётся глобально для всех компонентов.
add_compile_definitions(CONFIG_WIREGUARD_MAX_SRC_IPS=4)
project(bms_web)
