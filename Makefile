# =================================================================
# Makefile for CS744 Project (KV Server & Client)
# =================================================================

# --- Compiler and Flags ---
CXX = g++
# CXXFLAGS: -std=c++17, -g (debug symbols), -Wall (all warnings), -Wextra (more warnings)
CXXFLAGS = -std=c++17 -g -Wall -Wextra
# CPPFLAGS: -I tells the compiler where to find headers
CPPFLAGS = -Isrc/include
# LDFLAGS: Common libraries to link for both client and server
LDFLAGS = -lpthread -lssl -lcrypto

# --- Server Specific ---
SERVER_SRC = src/server/server.cpp src/server/db.cpp
SERVER_TARGET = build/server
# Server needs the mysql client library
SERVER_LIBS = -lmysqlclient

# --- Client Specific ---
CLIENT_SRC = src/client/client.cpp
CLIENT_TARGET = build/client
# Client has no extra libs
CLIENT_LIBS = 

# --- Build Targets ---
TARGET_DIR = build

# Phony targets don't represent files.
.PHONY: all clean

# --- Rules ---

# Default rule: 'make' or 'make all' will build both
all: $(TARGET_DIR) $(SERVER_TARGET) $(CLIENT_TARGET)

# Rule to build the server
$(SERVER_TARGET): $(SERVER_SRC) src/include/httplib.h
	@echo "Compiling server..."
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ -o $@ $(LDFLAGS) $(SERVER_LIBS)
	@echo "Server built: $@"

# Rule to build the client
$(CLIENT_TARGET): $(CLIENT_SRC) src/include/httplib.h
	@echo "Compiling client..."
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ -o $@ $(LDFLAGS) $(CLIENT_LIBS)
	@echo "Client built: $@"

# Rule to create the build directory
$(TARGET_DIR):
	mkdir -p $(TARGET_DIR)

# Rule to clean up build files
clean:
	@echo "Cleaning up build directory..."
	rm -rf $(TARGET_DIR)