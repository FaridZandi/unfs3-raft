set -euo pipefail

script_dir=$(realpath "$(dirname "$0")")
echo "grading in $script_dir"   

grade_results_path="$script_dir/grade_results/"
mkdir -p "$grade_results_path"

clientpath=/home/faridzandi/git/NfsClient-raft

./start-nfs-ext2.sh 5

cd $clientpath

python setup.py install

cd tests

test_results_path="$grade_results_path/test_raft.txt"
rm -f $test_results_path
touch $test_results_path

python main-a.py --file-count 20 --loop-delay 1 > "$test_results_path" 2>&1 &

# wait for a bit 
sleep 10

# the pid of the first replica can be found in inst1/unfsd.pid
replica1_pid=$(cat $script_dir/inst1/unfsd.pid)

echo "making replica 1 unresponsive"
kill -SIGUSR2 $replica1_pid
# wait for a bit
sleep 10

echo "making replica 1 responsive"
kill -SIGUSR1 $replica1_pid

# wait for the tests to finish
wait


sleep 10

cd $script_dir
./stop-nfs-ext2.sh