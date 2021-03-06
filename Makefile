


all:  __KERNEL_MOD__ __EMBC_RUNTIME__ __USER_APP__ __USER_EXT_TRIG_MOD__ __SOUND_SERVER__ __UTILS__ 
	@echo "*******************************************************************************"
	@echo "** FSMServer, SoundServer, RealtimeFSM.ko, UserSpaceExtTrig.ko,              **"
	@echo "** LynxTWO-RT.ko and LynxTrigRT.ko have been built.                          **"
	@echo "**                                                                           **"
	@echo "** If you want to compile the emulator, enter the 'emulator/' subdirectory   **"
	@echo "** and run './compile.sh'                                                    **"
	@echo "*******************************************************************************"

__EMBC_RUNTIME__:
	$(MAKE) -C runtime

__KERNEL_MOD__:
	$(MAKE) -C kernel && cp -f kernel/RealtimeFSM.ko .

__USER_APP__:
	$(MAKE) -C user && cp -f user/FSMServer .

__UTILS__:
	$(MAKE) -C utils

__USER_EXT_TRIG_MOD__:
	$(MAKE) -C addons/UserspaceExtTrig && cp -f addons/UserspaceExtTrig/UserspaceExtTrig.ko .

__SOUND_SERVER__:
	$(MAKE) -C addons/SoundTrig && cp -f addons/SoundTrig/SoundServer . && cp -f addons/SoundTrig/LynxTrig_RT.ko . && cp -f addons/SoundTrig/LynxTWO-RT/LynxTWO_RT.ko .

__EMULATOR__:
	cd emulator && ./compile.sh debug

__EMULATOR_CLEAN__:
	cd emulator && ./compile.sh debug clean
	
clean: 
	$(MAKE) -C kernel clean
	$(MAKE) -C runtime clean
	$(MAKE) -C user clean
	$(MAKE) -C utils clean
	$(MAKE) -C addons/UserspaceExtTrig/ clean
	$(MAKE) -C addons/SoundTrig/ clean
	rm -f RealtimeFSM.ko UserspaceExtTrig.ko FSMServer SoundServer LynxTrig_RT.ko LynxTWO_RT.ko *~ include/*~ both/*~
	find . -type f -name \*~ -exec rm -f {} \;
