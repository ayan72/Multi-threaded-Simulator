#include <stdio.h>
#include <iostream>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <map>
#include <sys/resource.h>
#include <sys/times.h>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <sstream>
#include <string>

using namespace std;

//macro definitions
#define NRES_TYPE 10
#define NTASKS 25
#define MAX_LEN 32

//defining the three possible states of the task
typedef enum {
    WAIT,
    RUN, 
    IDLE
    } status;

//struct to store the rescourses 
typedef struct {
    string name;
    int units;
    int held;
} Resources;

//represents a task which is executed by a thread
typedef struct {
    thread::id tid;
    string name;
    int counter;
    status stat;
    int busy_time;
    int idle_time;
    long wait_time;
    long run_time;
    vector<Resources> required_res;
} Task;

//global variables declaration
vector<Task> task;
int iter = 0;
long tick_rate = 0;
map<string, Resources> res_map;
map<string, Task> task_map;
vector<thread> thr;
bool monitor_running;
pthread_t taskThreadList[NTASKS];
mutex res_mutex;
mutex t_mutex;
mutex mon_mutex;

//convert the state types to their corresponding strings
string state_string(int state){
    switch(state){
        case WAIT:
            return "WAIT";
        case RUN:
            return "RUN";
        case IDLE:
            return "IDLE";
        default:
            return "UNKNOWN";
    }
}

//this function takes in the thread id and converts it into long unsigned int
long unsigned int tid_to_ul(thread::id tid){
    long unsigned int t_id;
    //https://en.cppreference.com/w/cpp/utility/hash
    t_id = hash<thread::id>{}(tid);
    return t_id;
}

//print_res() prints the resources which are available and held currently
void print_res(){
    printf("System Resources:\n");
    for(auto i: res_map){
        Resources res = i.second;
        printf("      %s: (maxAvial=   %d, held=   %d)\n",i.first.c_str(), res.units, res.held);
    }
}

//print_task takes in the number of iterations and prints the information about the tasks
void print_task(int iter){
    printf("System Tasks:\n");
    int cnt=0;

    for(auto i: task_map){
        Task t = i.second;
        printf("[%d] %s (%s, runTime= %d msec, idleTime= %d msec):\n",cnt,t.name.c_str(),state_string(t.stat).c_str(),t.busy_time,t.idle_time);
        //printing the tid in hexadecimal
        //https://stackoverflow.com/questions/14733761/printf-formatting-for-hexadecimal
        printf("      (tid = 0x%lx\n", tid_to_ul(t.tid));
        for (auto res: t.required_res){
            printf("      %s: (needed=   %d, held=   %d)\n",res.name.c_str(),res.units,res.held);
        }
        printf("      (RUN: %d times, WAIT: %ld msec)\n",iter,t.wait_time);
        cnt++;
    }

}

//delay function produces a delay of the period entered as the argument 
//using the nanosleep system call
void delay (long msec){
    struct timespec interval{
        .tv_sec = msec / 1000,
        .tv_nsec = (msec % 1000) * 1000000
    };
    //https://man7.org/linux/man-pages/man2/nanosleep.2.html
    if(nanosleep(&interval, NULL) < 0){
        perror("nanosleep error\n");
        exit(1);
    }
}

//clk_tick() returns the clock tick rate of the system
long clk_tick(){
    tick_rate = sysconf(_SC_CLK_TCK);
    if (errno){
        perror("sysconf error\n");
        exit(1);
    }
    return tick_rate;
}

/*/simulator is a task simulator which takes in the name of the task and number of iteration
it runs the iterations and uses mutex to maintain the synchronization and ensures shared resources are protected
*/
void simulator(string name, int iter) {
    Task current = task_map[name];
    task_map[name].tid = this_thread::get_id();
    task_map[name].counter += 1;

    for (int i=0; i<iter; i++){
        //https://cplusplus.com/reference/chrono/high_resolution_clock/now/
        auto start = chrono::high_resolution_clock::now();
    
        //update the status to WAIT
        t_mutex.lock();
        task_map[name].stat = WAIT;
        t_mutex.unlock();

        while(1){
            //https://en.cppreference.com/w/cpp/thread/mutex/try_lock
            if(res_mutex.try_lock()){
                bool lack_res = false;
                for (auto it: current.required_res){
                    int res = res_map[it.name].units - res_map[it.name].held;
                    if(res < it.units){
                        lack_res = true;
                        break;
                    }
                }
                if (lack_res){
                    //if there is a lack of resource, delay and try again
                    res_mutex.unlock();
                    delay(12);
                }
                else{
                    for(auto itr: current.required_res){
                        //resource available
                        res_map[itr.name].held += itr.units;
                    }
                    res_mutex.unlock();
                    break;
                }
            }
            else{
                delay(12);
            }
        }
        auto finish = chrono::high_resolution_clock::now();
        task_map[name].wait_time += chrono::duration_cast<chrono::milliseconds>(finish - start).count();

        //update the status of the task to RUN
        t_mutex.lock();
        task_map[name].stat = RUN;
        t_mutex.unlock();

        // simulating the run time
        delay(current.busy_time);

        for (auto res: current.required_res){
            //releasing the resource being held
            //https://en.cppreference.com/w/cpp/thread/unique_lock
            unique_lock<mutex> lock(res_mutex);
            auto it = res_map.find(res.name);
            if (it != res_map.end()){
                it->second.held -= res.units;
            }
        }

        //updating the status to Idle
        t_mutex.lock();
        task_map[name].stat = IDLE;
        t_mutex.unlock();

        //simulating the idle time
        delay(current.idle_time);
        long time_iter;
        time_iter += task_map[name].busy_time; 
        current.run_time = time_iter;
        printf("task: %s (tid= 0x%lx, iter= %d, time= %ld msec)\n", current.name.c_str(), tid_to_ul(current.tid), i+1, current.run_time);
        task_map[name].counter +=1;
        
    }
    
}

//monitor(int time) function is used to implement the monitor thread
//while the termination condition is false, it prints the output after a delay of the entered time period
//mutex lock and unlock is used to ensure the state is not changed while printing
void monitor(int time){
    while(monitor_running){
        res_mutex.lock();
        string wait_str = "    [WAIT]";
        string run_str = "    [RUN]";
        string idle_str = "    [IDLE]";
        for(auto i: task_map){
            Task tsk = i.second;
            switch(tsk.stat){
                case WAIT:
                    wait_str += tsk.name + " ";
                    break;
                case RUN:
                    run_str += tsk.name + " ";
                    break;
                case IDLE:
                    idle_str += tsk.name + " ";
                    break;
            }
        }
        res_mutex.unlock();
        printf("monitor:\n%s\n%s\n%s\n", wait_str.c_str(), run_str.c_str(), idle_str.c_str());
        delay(time);
    }

}

int main(int argc, char *argv[]) {
    //clock to record the start and end time
    tms start_time;
    clock_t start;
    start = times(&start_time);

    //checking for invalid inputs 
    if (argc != 4){
        printf("Invalid arguments entered \n");
    }

    char* inputFile = argv[1];
    int monitorTime = stoi(argv[2]);
    int niter = stoi(argv[3]);

    if (monitorTime < 0){
        printf("Moniter time cannot be negative\n");
        return 1;
    }

    if (niter < 0){
        printf("NITER cannot be negative\n");
        return 1;
    }

    //initializing an input stream
    ifstream in(inputFile);
    if (in.fail()){
        printf("unable to open input file\n");
        exit(1);
    }
    string line;
    if (in.good()){
        printf("Parsing the input file\n");
        while(getline(in,line)){
            if(line[0]== '#' || line.empty()){
                //skipping the comments and empty lines
                continue;
            }
            map<string, Resources> resource_map;
            map<string, Task> t_map;
            //https://www.geeksforgeeks.org/processing-strings-using-stdistringstream/
            istringstream iss(line);
            string type;
            iss >> type;

            if (strcmp(type.c_str(),"resources")==0){
                string st;
                while(iss >> st){
                    //finding the position of ':'
                    size_t pos = st.find(':');
                    if (pos != string::npos){
                        //storing the name and value pair in a map
                        string name = st.substr(0,pos);
                        int val = stoi(st.substr(pos+1));
                        res_map.insert({name,{name,val,0}});
                    }
                }
            }
            else if (strcmp(type.c_str(),"task")==0){
                //parsing the task line and storing the values in vector of task struct
                Task newTask;
                newTask.stat = IDLE;
                newTask.run_time = 0;

                iss >> newTask.name >> newTask.busy_time >> newTask.idle_time;

                string str;
                while(iss >> str){
                    size_t pos = str.find(':');
                    if (pos != string::npos){
                        string name = str.substr(0,pos);
                        int val = stoi(str.substr(pos+1));
                        newTask.required_res.push_back({name,val,0});
                    }
                }
                task_map.insert(std::make_pair(newTask.name, newTask));
            }
        }
    }
    printf("Successfully parsed the input file\n");
    in.close();

    monitor_running = true;

    //creating a thread for each task
    for(auto i: task_map){
        thr.push_back(thread(simulator,i.first,niter));
    }

    //creating the monitor thread
    thread mon_thread(monitor, monitorTime);

    //wait for all the threads to finish executing
    for(auto& t: thr){
        t.join();
    }

    //terminate the monitor thread and wait to it to finish executing
    monitor_running = false;
    mon_thread.join();

    //print the task and resource info
    print_res();
    print_task(niter);
    
    //calculate the total run time of the simulator in msec
    clock_t end;
    struct tms end_prog;
    end = times(&end_prog);
    long clockTick = clk_tick();
    long total_time = ((end - start)/clockTick)* 1000;

    cout<<"Running time= "<<total_time<< " msec"<<endl;

    return 0;
}