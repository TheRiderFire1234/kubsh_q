CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64
TARGET  := kubsh
SRC     := kubsh.c vfs.c

PKG_DIR := pkg
DEB     := kubsh_1.0.0_amd64.deb

# FUSE flags
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null || echo "-I/usr/include/fuse3")
FUSE_LIBS := $(shell pkg-config --libs fuse3 2>/dev/null || echo "-lfuse3")

.PHONY: all build clean run test deb

# Основная цель: собрать бинарник
all: build

build: $(SRC)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $(TARGET) $(SRC) $(FUSE_LIBS)

# Удобный запуск собранного бинарника
run: build
	./$(TARGET)

# Сборка deb-пакета
deb: build
	@echo "Сборка deb-пакета $(DEB)..."
	rm -rf $(PKG_DIR) $(DEB)
	mkdir -p $(PKG_DIR)/DEBIAN
	mkdir -p $(PKG_DIR)/usr/bin

	# Копируем бинарник
	cp $(TARGET) $(PKG_DIR)/usr/bin/$(TARGET)
	chmod 755 $(PKG_DIR)/usr/bin/$(TARGET)

	# Файл control с зависимостью от fuse3
	printf "Package: kubsh\nVersion: 1.0.0\nSection: utils\nPriority: optional\nArchitecture: amd64\nDepends: fuse3\nMaintainer: Unknown <root@localhost>\nDescription: kubsh shell with VFS\n kubsh is a shell with virtual file system capabilities.\n" > $(PKG_DIR)/DEBIAN/control

	# Сборка .deb
	dpkg-deb --build $(PKG_DIR) $(DEB)
	@echo "Готово: $(DEB)"

# Запуск тестов в Docker контейнере
# Тесты ищут kubsh в PATH, поэтому создаём симлинк в /usr/local/bin
test: build
	@echo "Запуск тестов в Docker контейнере..."
	@echo "Создаём симлинк kubsh в /usr/local/bin для доступа через PATH..."
	docker run -v $(PWD):/mnt tyvik/kubsh_test:master sh -c "ln -sf /mnt/kubsh /usr/local/bin/kubsh && chmod +x /usr/local/bin/kubsh && exec \$$@"

# Очистка артефактов сборки
clean:
	rm -f $(TARGET) $(DEB)
	rm -rf $(PKG_DIR)
