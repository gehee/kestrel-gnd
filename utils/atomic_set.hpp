#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <mutex>   // Required for std::mutex
#include <vector>
#include <chrono> // For std::chrono::milliseconds

template <typename T>

class AtomicSet {
private:
    std::set<T> mySet;
    mutable std::mutex mtx; // Use mutable if you want const methods to lock/unlock

public:
    bool insertIfNew(const T& value) {
        std::lock_guard<std::mutex> lock(mtx); // Even for read-only operations, if concurrent writes are possible
        if (mySet.count(value) > 0) {
            return false;
        }
        mySet.insert(value);
        if (mySet.size() > 20) {
            mySet.erase(mySet.begin());
        };
        return true;
    }


    void insert(const T& value) {
        std::lock_guard<std::mutex> lock(mtx); // Acquire lock upon construction, release upon destruction
        mySet.insert(value);
        if (mySet.size() > 20) {
            mySet.erase(mySet.begin());
        };
        // std::cout << "Inserted: " << value << std::endl; // Debugging: print with lock held
    }

    void erase(const T& value) {
        std::lock_guard<std::mutex> lock(mtx);
        mySet.erase(value);
        // std::cout << "Erased: " << value << std::endl; // Debugging: print with lock held
    }

    bool contains(const T& value) const {
        std::lock_guard<std::mutex> lock(mtx); // Even for read-only operations, if concurrent writes are possible
        return mySet.count(value) > 0;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx);
        return mySet.size();
    }

    // Example of safe iteration (copying to a local vector while locked)
    std::vector<T> get_elements_copy() const {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<T> elements_copy(mySet.begin(), mySet.end());
        return elements_copy;
    }
};