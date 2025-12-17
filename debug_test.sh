#!/bin/bash
# Скрипт для отладки тестов в Docker контейнере

echo "=== Проверка прав доступа ==="
docker run --rm -v $(pwd)/kubsh:/usr/bin/kubsh tyvik/kubsh_test:master sh -c "whoami && id"

echo ""
echo "=== Проверка возможности создания пользователя ==="
docker run --rm -v $(pwd)/kubsh:/usr/bin/kubsh tyvik/kubsh_test:master sh -c "useradd --help >/dev/null 2>&1 && echo 'useradd доступен' || echo 'useradd НЕ доступен'"

echo ""
echo "=== Тест создания пользователя ==="
TEST_USER="testuser_$$"
docker run --rm -v $(pwd)/kubsh:/usr/bin/kubsh tyvik/kubsh_test:master sh -c "
    useradd -m -s /bin/bash $TEST_USER 2>&1
    if id $TEST_USER >/dev/null 2>&1; then
        echo 'Пользователь создан успешно'
        userdel -r $TEST_USER 2>&1
    else
        echo 'Ошибка создания пользователя'
    fi
"

echo ""
echo "=== Запуск тестов с отладкой (stderr) ==="
docker run --rm -v $(pwd)/kubsh:/usr/bin/kubsh tyvik/kubsh_test:master 2>&1 | tee /tmp/kubsh_test_output.log | grep -E "(DEBUG|ERROR|FAILED|PASSED|test_vfs_add_user)" | head -30

echo ""
echo "=== Полный вывод теста test_vfs_add_user ==="
grep -A 20 "test_vfs_add_user" /tmp/kubsh_test_output.log | head -25

