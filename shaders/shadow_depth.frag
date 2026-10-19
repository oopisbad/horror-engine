#version 450 core

// Depth-only pass: nothing to write, the depth attachment is the product.
// (Explicit empty main keeps some drivers from optimizing the VS away.)
void main()
{
}
