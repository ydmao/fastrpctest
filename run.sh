for i in $(seq 1 16)
do
  #sudo ./obj/ibclient async &
  sudo ./obj/sync_client 192.168.100.11 &
  #echo $i
done
