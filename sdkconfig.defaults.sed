#
# SED (Sleepy End Device) defaults — use for Node V (valve board)
# Apply with: idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.sed" menuconfig
#
# Component config → OpenThread → Device Type → Minimal Thread Device (MTD)
CONFIG_OPENTHREAD_MTD=y
# CONFIG_OPENTHREAD_FTD is not set

# Component config → OpenThread → disable RX_ON_WHEN_IDLE (SED: radio off when idle)
# CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE is not set

# Component config → Power Management → Enable light sleep
CONFIG_PM_ENABLE=y
CONFIG_PM_LIGHT_SLEEP_ENABLE=y
