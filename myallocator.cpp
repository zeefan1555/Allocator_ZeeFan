#include "myallocator.h"
#include <vector>
using namespace std;

int main()
{
    vector<int, MyAllocator<int>> vec;

    for(int i = 0; i < 100; ++i)
    {
        int data = rand() % 1000;
        vec.push_back(data);

    }
    for(int val : vec)
    {
        cout<< val << " ";
    }
    cout<<endl;

    return 0;
}
