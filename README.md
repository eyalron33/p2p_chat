# p2p_chat
A simple p2p chat between two participants (planned to be extended to N participtans).

STUN information is generated via an external STUN server,  and is exchanged between participants via an external node.js server. Afterwards it's all p2p.

 # Build
 1. Compile [Socket.IO C++ Client](https://github.com/socketio/socket.io-client-cpp/)
 
 2. Put the following files into the 'object' folder:  libboost_date_time.a, libboost_random.a, libboost_system.a, libsioclient.a, libsioclient_tls.a
 
 3. build [libnice](https://nice.freedesktop.org/wiki/)
 
 4. execute: g++ -g -I ./header p2p-chat.cpp object/libsioclient.a object/libboost_date_time.a object/libboost_random.a object/libboost_system.a object/libsioclient_tls.a -o p2p-chat -std=gnu++11 `pkg-config --cflags --libs nice`
 
 # Run
 p2p-chat <nickname> $(host -4 -t A stun.stunprotocol.org | awk '{ print $4 }')
