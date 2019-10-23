{
	"targets": [
		{
			"target_name": "nodeAudioAsio",
			"sources": [ "./asio/host/pc/asiolist.cpp", "./asio/host/asiodrivers.cpp", "./asio/common/asio.cpp", "Source.cpp" ],
			"include_dirs": [ "asio", "asio/build", "asio/common", "asio/driver", "asio/host", "asio/host/pc", "<!(node -e \"require('nan')\")"],
		
		}
	],

}
