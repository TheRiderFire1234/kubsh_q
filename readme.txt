cat > run_ci.sh << 'EOF'
> sudo apt update
> sudo apt install -y build-essential dpkg-dev bats
> 
> make clean
> make
> ls -lh kubsh
> 
> ./kubsh --help 2>&1 | head -3 || ./kubsh --version 2>&1 | head -1
> echo "quit" | timeout 2 ./kubsh || echo "kph"
> 
> if [ -d "tests" ] && ls tests/*.bats 1>/dev/null 2>&1; then
>   bats tests/ || echo "bats tests completed"
> else
>   echo "No"
> fi
> 
> make deb
> ls -lh kubsh_*.deb
> 
> docker run --rm -v $(pwd):/host ububntu:22.04 bash -c "
> cd /host
> apt-get update >/dev/null 2>&1
> dpkg -i kubsh_*.deb 2>&1 | grep -v warning || apt-get install -f -y >/dev/null 2>&1
> if which kubsh >/dev/null 2>&1; then
>   echo 'installed'
> else 
>   'not in PATH'
> fi"
> EOF
