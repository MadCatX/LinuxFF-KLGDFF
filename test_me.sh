#! /bin/bash

(( ctr=0 ))

while (true)
do
  n=$RANDOM
  (( n=(n%9)+1 ))
  (( ctr=ctr+1 ))

  if [ $ctr -eq 10 ]; then
    echo ">> "$n
    (( ctr=0 ))
  else
    echo $n
  fi

  echo $n > /sys/kernel/klgdtm_obj/num
  sleep 0.1
done

