// transwarp is a header-only C++ library for task concurrency
// Version: in dev
// Author: Christian Blume (chr.blume@gmail.com)
// Repository: https://github.com/bloomen/transwarp
// License: http://www.opensource.org/licenses/mit-license.php
#pragma once
#include <future>
#include <type_traits>
#include <memory>
#include <tuple>
#include <string>
#include <cstddef>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <algorithm>
#include <queue>
#include <stdexcept>
#include <atomic>


namespace transwarp {


// forward declaration
class executor;


// A node carrying meta-data of a task
struct node {
    std::size_t id;
    std::size_t priority;
    std::size_t level;
    std::string name;
    std::vector<const node*> parents;
    transwarp::executor* executor;
};


// An edge between two nodes
struct edge {
    const transwarp::node* child;
    const transwarp::node* parent;
};


// An interface for gaining access to member functions that do not depend
// on the result type of the task
class iexecutable {
public:
    virtual ~iexecutable() = default;
    virtual void set_executor(std::shared_ptr<transwarp::executor> executor) = 0;
    virtual const transwarp::node& get_node() const = 0;
};


// An interface for the task class
template<typename ResultType>
class itask : public iexecutable {
public:
    virtual ~itask() = default;
    virtual std::shared_future<ResultType> get_future() const = 0;
};


// An interface for the final_task class
template<typename ResultType>
class ifinal_task : public itask<ResultType> {
public:
    virtual ~ifinal_task() = default;
    virtual void schedule(transwarp::executor* executor=nullptr) = 0;
    virtual void set_cancel(bool enabled) = 0;
    virtual const std::vector<transwarp::edge>& get_graph() const = 0;
};


// Base class for exceptions
class transwarp_error : public std::runtime_error {
public:
    explicit transwarp_error(const std::string& message)
    : std::runtime_error(message) {}
};


// Exception thrown when a task is canceled
class task_canceled : public transwarp::transwarp_error {
public:
    explicit task_canceled(const transwarp::node& n)
    : transwarp::transwarp_error(n.name + " is canceled") {}
};


namespace detail {


using callback_t = std::pair<std::function<void()>, const transwarp::node*>;

// A wrapper for a packager that is associated to a node. Sorting objects of
// this class will be first by node level, then by node priority, and
// finally by node id.
class wrapped_packager {
public:

    wrapped_packager(std::function<std::function<void()>()> packager, const transwarp::node* node)
    : packager_(std::move(packager)), node_(node) {}

    bool operator<(const wrapped_packager& other) const noexcept {
        constexpr const auto max = std::numeric_limits<std::size_t>::max();
        const auto lhs_pri = max - node_->priority;
        const auto rhs_pri = max - other.node_->priority;
        return std::tie(node_->level, lhs_pri, node_->id) <
               std::tie(other.node_->level, rhs_pri, other.node_->id);
    }

    transwarp::detail::callback_t make_callback() const {
        return std::make_pair(packager_(), node_);
    }

private:
    std::function<std::function<void()>()> packager_;
    const transwarp::node* node_;
};

// An exception for errors in the thread_pool class
class thread_pool_error : public transwarp::transwarp_error {
public:
    explicit thread_pool_error(const std::string& message)
    : transwarp::transwarp_error(message) {}
};

// A simple thread pool used to execute tasks in parallel
class thread_pool {
public:

    explicit thread_pool(std::size_t n_threads)
    : done_(false)
    {
        if (n_threads > 0) {
            const auto n_target = threads_.size() + n_threads;
            while (threads_.size() < n_target) {
                threads_.emplace_back(&thread_pool::worker, this);
            }
        } else {
            throw transwarp::detail::thread_pool_error("number of threads must be larger than zero");
        }
    }

    ~thread_pool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cond_var_.notify_all();
        for (auto& thread : threads_) {
            thread.join();
        }
    }

    void push(const std::function<void()>& functor) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            functors_.push(functor);
        }
        cond_var_.notify_one();
    }

private:

    void worker() {
        for (;;) {
            std::function<void()> functor;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cond_var_.wait(lock, [this]{
                    return done_ || !functors_.empty();
                });
                if (done_ && functors_.empty()) {
                    break;
                }
                functor = functors_.front();
                functors_.pop();
            }
            functor();
        }
    }

    bool done_;
    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> functors_;
    std::condition_variable cond_var_;
    std::mutex mutex_;
};

template<bool done, int total, int... n>
struct call_with_futures_impl {
    template<typename Result, typename Functor, typename Tuple>
    static Result work(Functor&& f, Tuple&& t) {
        return call_with_futures_impl<total == 1 + sizeof...(n), total, n..., sizeof...(n)>::template
                work<Result>(std::forward<Functor>(f), std::forward<Tuple>(t));
    }
};

template<int total, int... n>
struct call_with_futures_impl<true, total, n...> {
    template<typename Result, typename Functor, typename Tuple>
    static Result work(Functor&& f, Tuple&& t) {
        return std::forward<Functor>(f)(std::get<n>(std::forward<Tuple>(t)).get()...);
    }
};

// Calls the functor with the given tuple of futures. get() is called on every
// future and the results are then passed into the functor.
template<typename Result, typename Functor, typename Tuple>
Result call_with_futures(Functor&& f, Tuple&& t) {
    using tuple_t = typename std::decay<Tuple>::type;
    static const std::size_t n = std::tuple_size<tuple_t>::value;
    return transwarp::detail::call_with_futures_impl<0 == n, n>::template
            work<Result>(std::forward<Functor>(f), std::forward<Tuple>(t));
}

template<std::size_t...> struct indices {};

template<std::size_t...> struct construct_range;

template<std::size_t end, std::size_t idx, std::size_t... i>
struct construct_range<end, idx, i...> : construct_range<end, idx + 1, i..., idx> {};

template<std::size_t end, std::size_t... i>
struct construct_range<end, end, i...> {
    using type = transwarp::detail::indices<i...>;
};

template<std::size_t b, std::size_t e>
struct index_range {
    using type = typename transwarp::detail::construct_range<e, b>::type;
};

template<typename Functor, typename Tuple>
void call_with_each_index(transwarp::detail::indices<>, Functor&&, Tuple&&) {}

template<std::size_t i, std::size_t... j, typename Functor, typename Tuple>
void call_with_each_index(transwarp::detail::indices<i, j...>, Functor&& f, Tuple&& t) {
    auto ptr = std::get<i>(std::forward<Tuple>(t));
    if (!ptr) {
        throw transwarp::transwarp_error("Not a valid pointer to a task");
    }
    std::forward<Functor>(f)(*ptr);
    transwarp::detail::call_with_each_index(transwarp::detail::indices<j...>(), std::forward<Functor>(f), std::forward<Tuple>(t));
}

// Calls the functor with every element in the tuple. Expects the tuple to contain
// task pointers only and dereferences each element before passing it into the functor
template<typename Functor, typename Tuple>
void call_with_each(Functor&& f, Tuple&& t) {
    using tuple_t = typename std::decay<Tuple>::type;
    static const std::size_t n = std::tuple_size<tuple_t>::value;
    using index_t = typename transwarp::detail::index_range<0, n>::type;
    transwarp::detail::call_with_each_index(index_t(), std::forward<Functor>(f), std::forward<Tuple>(t));
}

template<int offset, typename... Tasks>
struct assign_futures_impl {
    static void work(const std::tuple<std::shared_ptr<Tasks>...>& source, std::tuple<std::shared_future<typename Tasks::result_type>...>& target) {
        std::get<offset>(target) = std::get<offset>(source)->get_future();
        assign_futures_impl<offset - 1, Tasks...>::work(source, target);
    }
};

template<typename... Tasks>
struct assign_futures_impl<-1, Tasks...> {
    static void work(const std::tuple<std::shared_ptr<Tasks>...>&, std::tuple<std::shared_future<typename Tasks::result_type>...>&) {}
};

// Returns the futures from the given tasks
template<typename... Tasks>
std::tuple<std::shared_future<typename Tasks::result_type>...> get_futures(const std::tuple<std::shared_ptr<Tasks>...>& input) {
    std::tuple<std::shared_future<typename Tasks::result_type>...> result;
    assign_futures_impl<static_cast<int>(sizeof...(Tasks)) - 1, Tasks...>::work(input, result);
    return result;
}

// Trims the given characters from the input string
inline std::string trim(const std::string &s, const std::string& chars=" \t\n\r") {
    auto functor = [&chars](char c) { return chars.find(c) != std::string::npos; };
    auto it = std::find_if_not(s.begin(), s.end(), functor);
    return std::string(it, std::find_if_not(s.rbegin(), std::string::const_reverse_iterator(it), functor).base());
}

// Sets level and parents of the node given in the constructor through
// the task given in the ()-operator
struct parent_visitor {
    explicit parent_visitor(transwarp::node& node) noexcept
    : node_(node) {}

    template<typename Task>
    void operator()(const Task& task) const {
        static_assert(!std::is_base_of<transwarp::ifinal_task<typename Task::result_type>, Task>::value,
                      "input task cannot be a final task");
        if (node_.level < task.node_.level)
            node_.level = task.node_.level;
        node_.parents.push_back(&task.node_);
    }

    transwarp::node& node_;
};

// Collects edges from the given node and task objects. The node in the
// constructor is the child and the task in the ()-operator is the parent.
struct edges_visitor {
    edges_visitor(std::vector<transwarp::edge>& graph, const transwarp::node& n) noexcept
    : graph_(graph), n_(n) {}

    template<typename Task>
    void operator()(const Task& task) const {
        graph_.push_back({&n_, &task.node_});
    }

    std::vector<transwarp::edge>& graph_;
    const transwarp::node& n_;
};

// Applies final bookkeeping to the task given in the ()-operator. This includes
// setting id, name, and canceled flag. Also, packagers and edges are collected.
struct final_visitor {
    final_visitor(std::vector<transwarp::detail::wrapped_packager>& packagers,
                  std::vector<transwarp::edge>& graph)
    : packagers_(packagers), graph_(graph), id_(0),
      canceled_(std::make_shared<std::atomic_bool>(false)) {}

    template<typename Task>
    void operator()(Task& task) {
        task.node_.id = id_++;
        if (task.node_.name.empty())
            task.node_.name = "task" + std::to_string(task.node_.id);
        packagers_.push_back(task.packager_);
        task.canceled_ = canceled_;
        transwarp::detail::call_with_each(transwarp::detail::edges_visitor(graph_, task.node_), task.parents_);
    }

    std::vector<transwarp::detail::wrapped_packager>& packagers_;
    std::vector<transwarp::edge>& graph_;
    std::size_t id_;
    std::shared_ptr<std::atomic_bool> canceled_;
};

// Visits the task given in the ()-operator using the visitors given in
// the constructor
template<typename PreVisitor, typename PostVisitor>
struct visit {
    visit(PreVisitor& pre_visitor, PostVisitor& post_visitor) noexcept
    : pre_visitor_(pre_visitor), post_visitor_(post_visitor) {}

    template<typename Task>
    void operator()(Task& task) const {
        task.visit(pre_visitor_, post_visitor_);
    }

    PreVisitor& pre_visitor_;
    PostVisitor& post_visitor_;
};

// Unvisits the task given in the ()-operator
struct unvisit {

    template<typename Task>
    void operator()(Task& task) const noexcept {
        task.unvisit();
    }
};


} // detail


// A visitor to be used to do nothing
struct pass_visitor {

    template<typename Task>
    void operator()(const Task&) const noexcept {}
};


// Creates a dot-style string from the given graph
inline std::string make_dot(const std::vector<transwarp::edge>& graph) {
    auto info = [](const transwarp::node& n) {
        const auto name = transwarp::detail::trim(n.name);
        return '"' + name + "\nid " + std::to_string(n.id)
                   + " pri " + std::to_string(n.priority)
                   + " lev " + std::to_string(n.level)
                   + " par " + std::to_string(n.parents.size()) + '"';
    };
    std::string dot = "digraph {\n";
    for (const auto& pair : graph) {
        dot += info(*pair.parent) + " -> " + info(*pair.child) + '\n';
    }
    dot += "}\n";
    return dot;
}


// The executor interface
class executor {
public:
    virtual ~executor() = default;
    virtual std::string get_name() const = 0;
    virtual void execute(const std::function<void()>& functor, const transwarp::node& node) = 0;
};


// Executor for sequential execution. Runs functors sequentially on the same thread
class sequential : public transwarp::executor {
public:

    std::string get_name() const override {
        return "transwarp::sequential";
    }

    void execute(const std::function<void()>& functor, const transwarp::node&) override {
        functor();
    }
};


// Executor for parallel execution. Uses a simple thread pool
class parallel : public transwarp::executor {
public:

    explicit parallel(std::size_t n_threads)
    : pool_(n_threads)
    {}

    std::string get_name() const override {
        return "transwarp::parallel";
    }

    void execute(const std::function<void()>& functor, const transwarp::node&) override {
        pool_.push(functor);
    }

private:
    transwarp::detail::thread_pool pool_;
};


// A task representing a piece work given by a functor and parent tasks.
// By connecting tasks a directed acyclic graph is built. Different priorities
// will affect the order of execution for tasks on the same level. Tasks with
// larger priorities will be executed first when at the same graph level.
template<typename Functor, typename... Tasks>
class task : public transwarp::itask<decltype(std::declval<Functor>()(std::declval<typename Tasks::result_type>()...))> {
public:
    // This is the result type of this task.
    // Getting a compiler error here means that the result types of the parent tasks
    // do not match or cannot be converted into the functor's parameters of this task
    using result_type = decltype(std::declval<Functor>()(std::declval<typename Tasks::result_type>()...));

    // A task is defined by name, priority, functor, and parent tasks
    // name and priority are optional. See constructor overloads
    task(std::string name, std::size_t priority, Functor functor, std::shared_ptr<Tasks>... parents)
    : node_{0, priority, 0, std::move(name), {}, nullptr},
      functor_(std::move(functor)),
      parents_(std::make_tuple(std::move(parents)...)),
      visited_(false),
      packager_(make_packager())
    {
        bookkeeping();
    }

    // This overload is for lowest priority of zero
    task(std::string name, Functor functor, std::shared_ptr<Tasks>... parents)
    : task(std::move(name), 0, std::move(functor), std::move(parents)...)
    {}

    // This overload is for auto-naming
    task(std::size_t priority, Functor functor, std::shared_ptr<Tasks>... parents)
    : task("", priority, std::move(functor), std::move(parents)...)
    {}

    // This overload is for auto-naming and lowest priority of zero
    task(Functor functor, std::shared_ptr<Tasks>... parents)
    : task("", 0, std::move(functor), std::move(parents)...)
    {}

    virtual ~task() = default;

    // delete copy/move semantics
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    task(task&&) = delete;
    task& operator=(task&&) = delete;

    // Assigns an executor to this task which takes precedence over
    // the global executor provided in final_task::schedule()
    void set_executor(std::shared_ptr<transwarp::executor> executor) override {
        if (!executor) {
            throw transwarp::transwarp_error("Not a valid pointer to executor");
        }
        executor_ = std::move(executor);
        node_.executor = executor_.get();
    }

    // Returns the future associated to the underlying execution
    std::shared_future<result_type> get_future() const override {
        std::lock_guard<std::mutex> lock(future_mutex_);
        return future_;
    }

    // Returns the associated node
    const transwarp::node& get_node() const override {
        return node_;
    }

    // Returns the functor
    const Functor& get_functor() const {
        return functor_;
    }

    // Returns the parent tasks
    const std::tuple<std::shared_ptr<Tasks>...>& get_parents() const {
        return parents_;
    }

    // Visits each task in a depth-first traversal. The pre_visitor is called
    // before traversing through parents and the post_visitor after. A visitor
    // takes a reference to a task (task&) as its only input argument.
    template<typename PreVisitor, typename PostVisitor>
    void visit(PreVisitor& pre_visitor, PostVisitor& post_visitor) {
        if (!visited_) {
            pre_visitor(*this);
            transwarp::detail::call_with_each(transwarp::detail::visit<PreVisitor, PostVisitor>(pre_visitor, post_visitor), parents_);
            post_visitor(*this);
            visited_ = true;
        }
    }

    // Traverses through all tasks and marks them as not visited.
    void unvisit() noexcept {
        if (visited_) {
            visited_ = false;
            transwarp::detail::call_with_each(transwarp::detail::unvisit(), parents_);
        }
    }

private:

    friend struct transwarp::detail::parent_visitor;
    friend struct transwarp::detail::edges_visitor;
    friend struct transwarp::detail::final_visitor;

    // Calls the functor of the given task with the results from the futures.
    // Throws transwarp::task_canceled if the task is canceled.
    static result_type evaluate(transwarp::task<Functor, Tasks...>& task,
                                std::tuple<std::shared_future<typename Tasks::result_type>...> futures) {
        if (*task.canceled_)
            throw transwarp::task_canceled(task.get_node());
        return transwarp::detail::call_with_futures<result_type>(task.functor_, std::move(futures));
    }

    // Creates a wrapped packager. Calling the packager will create a packaged
    // task given the parent futures, then assign a new future to this task
    // and finally return a callback to run the packaged task.
    transwarp::detail::wrapped_packager make_packager() {
        auto packager = [this] {
            auto futures = transwarp::detail::get_futures(parents_);
            auto pack_task = std::make_shared<std::packaged_task<result_type()>>(
                    std::bind(&task::evaluate, std::ref(*this), std::move(futures)));
            {
                std::lock_guard<std::mutex> lock(future_mutex_);
                future_ = pack_task->get_future();
            }
            return [pack_task] { (*pack_task)(); };
        };
        return {packager, &node_};
    }

    // Assigns level and parents of this task via the node object
    void bookkeeping() {
        transwarp::detail::call_with_each(transwarp::detail::parent_visitor(node_), parents_);
        if (sizeof...(Tasks) > 0)
            ++node_.level;
    }

    transwarp::node node_;
    Functor functor_;
    std::tuple<std::shared_ptr<Tasks>...> parents_;
    bool visited_;
    transwarp::detail::wrapped_packager packager_;
    std::shared_ptr<transwarp::executor> executor_;
    std::shared_ptr<std::atomic_bool> canceled_;
    std::shared_future<result_type> future_;
    mutable std::mutex future_mutex_;
};


// The final task is the very last task in the graph. The final task has no children.
// Depending on how tasks in the graph are arranged they can be run in parallel
// by design if a parallel executor is used. Tasks may run in parallel when they
// do not depend on each other, i.e., they are independent with respect to the
// task that consumes them.
template<typename Functor, typename... Tasks>
class final_task : public transwarp::task<Functor, Tasks...>,
                   public transwarp::ifinal_task<typename transwarp::task<Functor, Tasks...>::result_type> {
public:
    // This is the result type of this final task.
    // Getting a compiler error here means that the result types of the parent tasks
    // do not match or cannot be converted into the functor's parameters of this task
    using result_type = typename transwarp::task<Functor, Tasks...>::result_type;

    // A final task is defined by name, functor, and parent tasks.
    final_task(std::string name, Functor functor, std::shared_ptr<Tasks>... parents)
    : transwarp::task<Functor, Tasks...>(std::move(name), std::move(functor), std::move(parents)...)
    {
        finalize();
    }

    // This overload is for auto-naming
    final_task(Functor functor, std::shared_ptr<Tasks>... parents)
    : transwarp::final_task<Functor, Tasks...>("", std::move(functor), std::move(parents)...)
    {}

    virtual ~final_task() = default;

    // delete copy/move semantics
    final_task(const final_task&) = delete;
    final_task& operator=(const final_task&) = delete;
    final_task(final_task&&) = delete;
    final_task& operator=(final_task&&) = delete;

    // Assigns an executor to this task which takes precedence over
    // the global executor provided in final_task::schedule()
    void set_executor(std::shared_ptr<transwarp::executor> executor) override {
        transwarp::task<Functor, Tasks...>::set_executor(std::move(executor));
    }

    // Returns the future associated to the underlying execution
    std::shared_future<result_type> get_future() const override {
        return transwarp::task<Functor, Tasks...>::get_future();
    }

    // Returns the associated node
    const transwarp::node& get_node() const override {
        return transwarp::task<Functor, Tasks...>::get_node();
    }

    // Schedules the graph for execution using the provided executor.
    // The task-specific executor gets precedence if it exists.
    // Complexity is O(n) with n being the number of tasks in the graph.
    // The callbacks are packaged tasks that are ordered first by level, then
    // priority, and finally ID. Throws transwarp_error if neither the global
    // nor a task-specific executor is found.
    void schedule(transwarp::executor* executor=nullptr) override {
        if (!*canceled_) {
            prepare_callbacks();
            for (const auto& callback : callbacks_) {
                auto exec = callback.second->executor;
                if (!exec && executor) {
                    exec = executor;
                }
                if (!exec) {
                    throw transwarp::transwarp_error("No valid executor for task: " + callback.second->name);
                }
                exec->execute(callback.first, *callback.second);
            }
        }
    }

    // If enabled then all pending tasks are canceled which will
    // throw transwarp::task_canceled when asking a future for its result.
    // Canceling pending tasks does not affect currently running tasks.
    // As long as cancel is enabled new computations cannot be scheduled.
    void set_cancel(bool enabled) override {
        *canceled_ = enabled;
    }

    // Returns the graph of the task structure. This is mainly for visualizing
    // the tasks and their interdependencies. Pass the result into transwarp::make_dot
    // to retrieve a dot-style graph representation for easy viewing.
    const std::vector<transwarp::edge>& get_graph() const override {
        return graph_;
    }

private:

    // Finalizes the graph of tasks by computing IDs, collecting the packager of
    // each task, populating a vector of edges, etc. The packagers are then
    // sorted by level, priority, and ID which ensures that tasks higher in the
    // graph are executed first.
    void finalize() {
        transwarp::pass_visitor pass;
        transwarp::detail::final_visitor post_visitor(packagers_, graph_);
        canceled_ = post_visitor.canceled_;
        this->visit(pass, post_visitor);
        this->unvisit();
        callbacks_.resize(packagers_.size());
        std::sort(packagers_.begin(), packagers_.end());
    }

    // Calls all packagers and stores the results as callbacks. Every task has
    // a packager which, when called, wraps the task and assigns a new future.
    // Calling the callback will then actually run the functor associated to the
    // task and store the result in the future. The callbacks are dealt with
    // by the schedule function.
    void prepare_callbacks() {
        std::transform(packagers_.begin(), packagers_.end(), callbacks_.begin(),
                       [](const transwarp::detail::wrapped_packager& p) {
                           return p.make_callback();
                       });
    }

    std::shared_ptr<std::atomic_bool> canceled_;
    std::vector<transwarp::detail::wrapped_packager> packagers_;
    std::vector<transwarp::detail::callback_t> callbacks_;
    std::vector<transwarp::edge> graph_;
};


// A factory function to create a new task
template<typename Functor, typename... Tasks>
std::shared_ptr<transwarp::task<Functor, Tasks...>> make_task(std::string name, std::size_t priority, Functor functor, std::shared_ptr<Tasks>... parents) {
    return std::make_shared<transwarp::task<Functor, Tasks...>>(std::move(name), priority, std::move(functor), std::move(parents)...);
}

// A factory function to create a new task. Overload for lowest priority of zero
template<typename Functor, typename... Tasks>
std::shared_ptr<transwarp::task<Functor, Tasks...>> make_task(std::string name, Functor functor, std::shared_ptr<Tasks>... parents) {
    return std::make_shared<transwarp::task<Functor, Tasks...>>(std::move(name), std::move(functor), std::move(parents)...);
}

// A factory function to create a new task. Overload for auto-naming
template<typename Functor, typename... Tasks>
std::shared_ptr<transwarp::task<Functor, Tasks...>> make_task(std::size_t priority, Functor functor, std::shared_ptr<Tasks>... parents) {
    return std::make_shared<transwarp::task<Functor, Tasks...>>(priority, std::move(functor), std::move(parents)...);
}

// A factory function to create a new task. Overload for auto-naming and lowest priority of zero
template<typename Functor, typename... Tasks>
std::shared_ptr<transwarp::task<Functor, Tasks...>> make_task(Functor functor, std::shared_ptr<Tasks>... parents) {
    return std::make_shared<transwarp::task<Functor, Tasks...>>(std::move(functor), std::move(parents)...);
}


// A factory function to create a new final task
template<typename Functor, typename... Tasks>
std::shared_ptr<transwarp::final_task<Functor, Tasks...>> make_final_task(std::string name, Functor functor, std::shared_ptr<Tasks>... parents) {
    return std::make_shared<transwarp::final_task<Functor, Tasks...>>(std::move(name), std::move(functor), std::move(parents)...);
}

// A factory function to create a new final task. Overload for auto-naming
template<typename Functor, typename... Tasks>
std::shared_ptr<transwarp::final_task<Functor, Tasks...>> make_final_task(Functor functor, std::shared_ptr<Tasks>... parents) {
    return std::make_shared<transwarp::final_task<Functor, Tasks...>>(std::move(functor), std::move(parents)...);
}


} // transwarp
