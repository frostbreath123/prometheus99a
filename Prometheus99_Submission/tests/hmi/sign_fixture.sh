# Sourced by the executable tests. Signs "$2/cfg.json" with a fresh key.
#   sign_config <build dir> <work dir>
# Afterwards pass:  --config-key "$2/cfg.key" --config-state "$2/cfg.serial"
sign_config() {
  "$1/p99_sign" keygen --key "$2/cfg.key" > /dev/null || return 1
  "$1/p99_sign" sign --config "$2/cfg.json" --key "$2/cfg.key" > /dev/null || return 1
}
