#!/bin/sh

. /usr/share/libubox/jshn.sh
. /lib/functions.sh

function get_wlan_temperature()
{
  :
}

function get_sfp_temperature()
{
  json_add_object
  json_add_string "name" "sfp"
  json_add_int "temperature" "-274"
  json_close_object
}

function get_cpu_temperature()
{
  json_add_object
  json_add_string "name" "cpu"
  json_add_int "temperature" "-274"
  json_close_object
}

function get_temperature_status()
{
  local hasWifi="$(db -q get hw.board.hasWifi)"
  local hasSfp="$(db -q get hw.board.hasSfp)"

  json_init
  json_add_array "status"
    get_cpu_temperature
    [ "$hasSfp" = "1" ] && get_sfp_temperature
    [ "$hasWifi" = "1" ] && get_wlan_temperature
  json_close_array

  json_dump
}

function dump_invalid()
{
  json_init
  json_add_string "fault" "invalid request"
  json_dump
}

case "$1" in
    status)
        get_temperature_status
        ;;
    *)
        dump_invalid
        ;;
esac
