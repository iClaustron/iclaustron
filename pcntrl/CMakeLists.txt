add_executable(ic_pcntrld ic_pcntrl.c)
target_link_libraries(ic_pcntrld comm ic_util ic_port ic_apic ic_apid)
