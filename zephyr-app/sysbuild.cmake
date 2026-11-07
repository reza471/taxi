# Refuse to build a firmware that trusts the PUBLIC test key of the MCUboot project:
# anybody could sign an update for such a meter.
if(NOT TAXI_ALLOW_DEV_KEY)
  if(NOT SB_CONFIG_BOOT_SIGNATURE_KEY_FILE OR SB_CONFIG_BOOT_SIGNATURE_KEY_FILE MATCHES "root-")
    message(FATAL_ERROR
      "No private signing key given. Create one and pass it:\n"
      "  python3 <mcuboot>/scripts/imgtool.py keygen -k taxi_ota_key.pem -t ecdsa-p256\n"
      "  west build --sysbuild ... -- -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\\\"/path/taxi_ota_key.pem\\\"\n"
      "(keep the key OFFLINE and OUT of git; for a bench test only: -DTAXI_ALLOW_DEV_KEY=1)")
  endif()
endif()
