cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(taxi_meter)

target_include_directories(app PRIVATE core src)
target_sources(app PRIVATE
	core/fare_engine.c
	core/json_lite.c
	core/urlq.c
	src/main.c
	src/pulse.c
	src/keys.c
	src/display.c
	src/ui.c
	src/clock.c
	src/meter.c
	src/beep.c
	src/persist.c
	src/tariff.c
	src/net.c
	src/postq.c
	src/trip_store.c
	src/report.c
	src/wdog.c
)
