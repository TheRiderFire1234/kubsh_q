CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=35
TARGET  := kubsh
SRCS    := kubsh.c vfs.c
OBJS    := $(SRCS:.c=.o)

LIBS    := -lfuse3 -lpthread

PKG_DIR := pkg
DEB     := kubsh_1.0.0_amd64.deb

.PHONY: all build clean run test deb

all: build

# Явное правило: собрать бинарник из исходников
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

build: $(TARGET)

run: build
	./$(TARGET)

deb: build
	@echo "Сборка deb-пакета $(DEB)..."
	rm -rf $(PKG_DIR) $(DEB)
	mkdir -p $(PKG_DIR)/DEBIAN
	mkdir -p $(PKG_DIR)/usr/bin

	cp $(TARGET) $(PKG_DIR)/usr/bin/$(TARGET)
	chmod 755 $(PKG_DIR)/usr/bin/$(TARGET)

	printf "Package: kubsh\nVersion: 1.0.0\nSection: utils\nPriority: optional\nArchitecture: amd64\nDepends: libfuse3-3 (>= 3.0.0)\nMaintainer: Unknown <root@localhost>\nDescription: kubsh shell with FUSE-based VFS exposing /opt/users\n" > $(PKG_DIR)/DEBIAN/control

	dpkg-deb --build $(PKG_DIR) $(DEB)
	@echo "Готово: $(DEB)"

test: build
	@echo "Запуск тестов..."
	docker run --rm --cap-add SYS_ADMIN --device /dev/fuse -v $(PWD)/kubsh:/usr/bin/kubsh tyvik/kubsh_test:master

clean:
	rm -f $(TARGET) $(DEB) *.o
	rm -rf $(PKG_DIR)
