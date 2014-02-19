for i in $(seq 1 16)
do
  #sudo ./obj/ibclient async &
  sudo ./obj/sync_client &
  #echo $i
done
