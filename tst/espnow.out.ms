[drv 0ms] espnow.init
mac	24:0a:c4:00:00:01	1
[drv 0ms] espnow.set_channel 6
channel	6
recv	24:0a:c4:05:06:07	PING from afar	14	300
[drv 300ms] espnow.add_peer 24:0a:c4:05:06:07
[drv 300ms] espnow.send 24:0a:c4:05:06:07 PONG
send	24:0a:c4:05:06:07	success
catch	false	espnow.send : esp_now_send failed: ESP_ERR_ESPNOW_NOT_FOUND
catch	false	espnow.set_channel : invalid Wi-Fi channel (1-14)
