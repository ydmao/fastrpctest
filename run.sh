for i in $(seq 1 16)
do
  sudo ./obj/ibclient async &
  #echo $i
done
