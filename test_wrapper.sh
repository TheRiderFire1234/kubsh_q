#!/bin/sh
# Обёртка для запуска тестов в Docker контейнере
# Создаёт симлинк kubsh в /usr/local/bin перед запуском тестов

set -e

# Создаём симлинк kubsh в /usr/local/bin (который точно в PATH)
ln -sf /mnt/kubsh /usr/local/bin/kubsh
chmod +x /usr/local/bin/kubsh

# Проверяем, что kubsh доступен
if ! command -v kubsh >/dev/null 2>&1; then
    echo "Ошибка: kubsh не найден в PATH после создания симлинка" >&2
    exit 1
fi

echo "kubsh установлен в PATH: $(which kubsh)"
echo "Запуск тестов..."

# Запускаем команду по умолчанию контейнера
exec "$@"

