target_sources(${PROJECT_NAME} PRIVATE
        maple_cfg.cpp
        maple_cfg.h
        maple_devs.cpp
        maple_devs.h
        maple_helper.cpp
        maple_helper.h
        maple_if.cpp
        maple_if.h
        maple_jvs.cpp)

target_sources(${PROJECT_NAME} PRIVATE
        vmu_cpu.cpp
        vmu_cpu.h
        vmu_emulator.cpp
        vmu_emulator.h
        vmu_memory.cpp
        vmu_memory.h
        vmu_pic.cpp
        vmu_pic.h
        vmu_timers.cpp
        vmu_timers.h)
