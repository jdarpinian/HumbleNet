CMAKE_MINIMUM_REQUIRED(VERSION 3.5)

CreateTool(APIBuilder
DIRS
	.
INCLUDES
FEATURES
	cxx_auto_type cxx_range_for
LINK
	jsonparser
PROPERTIES
	FOLDER HumbleNet/Tools
)
