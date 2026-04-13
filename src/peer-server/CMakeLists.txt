CMAKE_MINIMUM_REQUIRED(VERSION 3.5)


CreateTool(peer-server
DIRS
	.
FEATURES
	cxx_auto_type cxx_range_for
INCLUDES
	${BoringSSL_INCLUDE_DIR}
LINK
	humblepeer
	sha1
	websockets
	${CMAKE_THREAD_LIBS_INIT}
PROPERTIES
	FOLDER HumbleNet/Server
	CXX_STANDARD 14
)


source_group("Source" REGULAR_EXPRESSION ".")
source_group("" FILES CMakeLists.txt)
