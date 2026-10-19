This directory holds writable runtime state (Linux FHS /var/lib):

  battery_history.dat   - battery charge-history records (Settings app)
  screen_usage.dat      - daily screen-on usage (optimized charging)
  wake_log.bin          - persistent light-sleep wake log

It is (re)created at boot by app_config_init() because SPIFFS partition
images do not persist empty directories.
