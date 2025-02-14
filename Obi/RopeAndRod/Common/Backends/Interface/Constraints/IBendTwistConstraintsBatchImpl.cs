﻿using UnityEngine;
using System.Collections;

namespace Obi
{
    public interface IBendTwistConstraintsBatchImpl : IConstraintsBatchImpl
    {
        void SetBendTwistConstraints(ObiNativeIntList orientationIndices, ObiNativeQuaternionList restDarboux, ObiNativeVector3List stiffnesses, ObiNativeVector2List plasticity, ObiNativeFloatList lambdas, int count);
    }
    
    public interface IPDBendTwistConstraintsBatchImpl : IConstraintsBatchImpl
    {
        void SetBendTwistConstraints(ObiNativeIntList orientationIndices, ObiNativeFloatList restLengths,ObiNativeQuaternionList restDarboux, ObiNativeVector3List stiffnesses, ObiNativeVector2List plasticity, ObiNativeFloatList lambdas, int count);
    }
}
