#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;  //数据库
    int m_actor_model;          //模型切换
};
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)  // 线程小于0或者请求小于0
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];  //只是创建了一个线程数组，大小为number。还有没有生成线程。
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)  //逐个创建线程
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)  //线程创建调用库函数  
        {           //传入线程的位置（指向这个线程的指针），线程的属性(如线程中的堆栈大小等等，默认为NULL)，线程入口函数，总得运行东西把。worker需要的参数。
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))  //所有子线程去后台执行？脱离主线程。哦哦，后面会唤醒他。
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;    //最后肯定是释放他了。
}
template <typename T>
bool threadpool<T>::append(T *request, int state)  //需求，状态,reactor
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)  //先上锁操作，发现当前任务队列已经达到最大值，解锁，添加不了。
    {
        m_queuelocker.unlock();
        return false;
    }                                
    request->m_state = state;    //state是读写，1是写，0是读/能添加了，添加状态，放需求，解锁，work队列中有新任务了，用信号量去唤醒线程上班。
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
bool threadpool<T>::append_p(T *request)   //只有需求。proactor 不需要参数，？
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)  //线程入口函数
{
    threadpool *pool = (threadpool *)arg;  //转换为threadpool类型，然后可以调用run方法把。
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run() //线程真正的工作地方
{
    while (true)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        if (1 == m_actor_model)   //reactor   工作线程要负责读写。
        {
            if (0 == request->m_state)  // 是读.
            {
                if (request->read_once())  //要读完还是读一次，取决于et还是lt  //读完了进行这里，没读完出错了下面的。
                {
                    request->improv = 1;  // 改过了。
                    connectionRAII mysqlcon(&request->mysql, m_connPool);  //来一个连接。 这个mysql就是给了一个连接
                    request->process();   //逻辑处理。  process中需要用到mysql，就是他。
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;     // 干完活了，就默认到时间了？让滚蛋？
                }
            }
            else                     //是写
            {
                if (request->write())   //写完了而且不要断开
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;  //没写完或者写完了需要断开。 给个timer_flag
                    request->timer_flag = 1;// 干完活了，就默认到时间了？让滚蛋？
                }
            }
        }
        else   //是proactor ，直接交给process。 因为主线程已经完成了读。
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();  //如果是proactor，工作线程干完了，我只管逻辑。
        }              
    }
}
#endif
