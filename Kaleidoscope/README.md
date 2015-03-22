#Kaleidoscope

[Kaleidoscope: Implementing a Language with LLVM](http://llvm.org/docs/tutorial/index.html)

#1 Tutorial Introduction and the Lexer

implementing tokens and lexer

#2 Implementing a Parser and AST

implementing AST and parser.

##2.5 Binary Expression Parsing

This is the most interesting part of this chapter.

#3 Code generation to LLVM IR

To compile the code, it requires `apt-get install libedit-dev`.

This chapter is beginning to talk about LLVM.

#4 Adding JIT and Optimizer Support

The original code cannot be compiled, because it is too old. I modified the code and make it pass the compilation, but it still doesn't work. And I have checked the code in the package `llvm-3.5-examples`, it also have the same issues. So I give up, and just go through the rest chapters. I may use the book named "Getting Started with LLVM Core Libraries" to learn LLVM.
