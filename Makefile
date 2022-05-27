#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := espnow_example

include $(IDF_PATH)/make/project.mk


PORTS:= 0 1 2 3

build: 
	/home/shayn/.espressif/python_env/idf4.4_py3.8_env/bin/python /home/shayn/esp/esp-idf/tools/idf_size.py /home/shayn/vscode/ESPMesh/build/ESPMesh.map

flashx: $(PORTS)
$(PORTS): 
	/home/shayn/.espressif/python_env/idf4.4_py3.8_env/bin/python /home/shayn/esp/esp-idf/components/esptool_py/esptool/esptool.py -p /dev/ttyUSB$@ -b 460800 --before default_reset --after hard_reset --chip esp32 write_flash --flash_mode dio --flash_freq 40m --flash_size detect 0x10000 ESPMesh.bin 0x1000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin

flash: $(PORTS)
$(PORTS): 
	@echo /home/shayn/.espressif/python_env/idf4.4_py3.8_env/bin/python /home/shayn/esp/esp-idf/components/esptool_py/esptool/esptool.py -p /dev/ttyUSB$@ -b 460800 --before default_reset --after hard_reset --chip esp32 write_flash --flash_mode dio --flash_freq 40m --flash_size detect 0x10000 ESPMesh.bin 0x1000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin


monitor: $(PORTS)
$(PORTS):
	@echo /home/shayn/.espressif/python_env/idf4.4_py3.8_env/bin/python /home/shayn/esp/esp-idf/tools/idf.py -p /dev/$@ monitor


flash:$(PORTS)
$(PORTS):
	@echo /home/shayn/.espressif/python_env/idf4.4_py3.8_env/bin/python /home/shayn/esp/esp-idf/components/esptool_py/esptool/esptool.py -p /dev/ttyUSB$@ -b 460800 --before default_reset --after hard_reset --chip esp32 write_flash --flash_mode dio --flash_freq 40m --flash_size detect 0x10000 ESPMesh.bin 0x1000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin


flashxx:
	@echo /home/shayn/.espressif/python_env/idf4.4_py3.8_env/bin/python /home/shayn/esp/esp-idf/components/esptool_py/esptool/esptool.py -p /dev/ttyUSB$(PORT) -b 460800 --before default_reset --after hard_reset --chip esp32 write_flash --flash_mode dio --flash_freq 40m --flash_size detect 0x10000 ESPMesh.bin 0x1000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin


TASKS = check test release

.PHONY: $(TASKS)
$(TASKS):
	@echo @poetry run duty $@