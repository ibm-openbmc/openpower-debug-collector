# SPDX-License-Identifier: Apache-2.0

unit_files += [
        ['dump/org.open_power.Dump.Manager.service',
         'org.open_power.Dump.Manager.service'],
]

cxx = meson.get_compiler('cpp')

dump_deps = [
    systemd,
    sdbusplus,
    phosphorlogging,
    cxx.find_library('sdeventplus'),
    cxx.find_library('pdbg'),
    cxx.find_library('libdt-api'),
]

# Configuration header file(dump_config.h) generation
dump_conf_data = configuration_data()
dump_conf_data.set_quoted('OP_DUMP_OBJPATH', get_option('OP_DUMP_OBJPATH'),
                     description : 'The D-Bus root of openpower-dump-collector')
dump_conf_data.set_quoted('OP_DUMP_BUSNAME', get_option('OP_DUMP_BUSNAME'),
                     description : 'The bus name of openpower-dump-collector')
dump_conf_data.set_quoted('HB_DUMP_DBUS_OBJPATH', get_option('HB_DUMP_DBUS_OBJPATH'),
                     description : 'The hostboot dump manager path')
dump_conf_data.set_quoted('HB_DUMP_COLLECTION_PATH', get_option('HB_DUMP_COLLECTION_PATH'),
                     description : 'The path to store collected hostboot dump files')
configure_file(configuration : dump_conf_data,
               output : 'config.h')

# source files
dump_src = files(
    'dump_manager.cpp',
    'dump_utils.cpp',
    'dump_manager_main.cpp',
)

executable('openpower-dump-collector',
    dump_src,
    dependencies: dump_deps,
    implicit_include_directories: true,
    install: true
)