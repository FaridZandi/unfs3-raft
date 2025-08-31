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

# wait for the election to settle
echo "waiting for 2 seconds to let the election settle"
sleep 2

echo "running tests, output in $test_results_path"
python -u main-a.py --file-count 40 --loop-delay 1 | tee -a "$test_results_path" &

echo "waiting for 10 seconds to let tests start"
sleep 5


# TODO: there's no guarantee that replica 1 is the leader. 
# the pid of the first replica can be found in inst1/unfsd.pid
replica1_pid=$(cat $script_dir/inst1/unfsd.pid)
echo "making replica 1 unresponsive"
kill -SIGUSR2 $replica1_pid


# wait for a bit
sleep 5

echo "making replica 1 responsive"
kill -SIGUSR1 $replica1_pid

# wait for the tests to finish
wait

# prompt before stopping nfs
echo "Tests finished. Press Enter to stop nfs and clean up"
read -r

cd $script_dir
./stop-nfs-ext2.sh