class TachoData
{
public:
    // Data members
    float Cost_1PM;
    float Cost_1s;
    float Sum_Cost1PM[4];
    float Sum_Cost1s[4];
    long int StopSec[4];
    float Sum_Cost[4] = {0};
    float Pri_Cost;
    float Prim_Cost;
    float Final_Cost[4];

    // Constructor
    TachoData()
    {
        // Optionally initialize members if needed
        Cost_1PM = 0.0;
        Cost_1s = 0.0;
        Pri_Cost = 0.0;
        Prim_Cost = 0.0;
        for (int i = 0; i < 4; i++)
        {
            Sum_Cost1PM[i] = 0.0;
            Sum_Cost1s[i] = 0.0;
            StopSec[i] = 0;
            Sum_Cost[i] = 0.0;
            Final_Cost[i] = 0.0;
        }
    }
};
