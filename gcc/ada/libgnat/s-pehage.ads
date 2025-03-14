------------------------------------------------------------------------------
--                                                                          --
--                         GNAT COMPILER COMPONENTS                         --
--                                                                          --
--        S Y S T E M . P E R F E C T _ H A S H _ G E N E R A T O R S       --
--                                                                          --
--                                 S p e c                                  --
--                                                                          --
--                     Copyright (C) 2002-2021, AdaCore                     --
--                                                                          --
-- GNAT is free software;  you can  redistribute it  and/or modify it under --
-- terms of the  GNU General Public License as published  by the Free Soft- --
-- ware  Foundation;  either version 3,  or (at your option) any later ver- --
-- sion.  GNAT is distributed in the hope that it will be useful, but WITH- --
-- OUT ANY WARRANTY;  without even the  implied warranty of MERCHANTABILITY --
-- or FITNESS FOR A PARTICULAR PURPOSE.                                     --
--                                                                          --
-- As a special exception under Section 7 of GPL version 3, you are granted --
-- additional permissions described in the GCC Runtime Library Exception,   --
-- version 3.1, as published by the Free Software Foundation.               --
--                                                                          --
-- You should have received a copy of the GNU General Public License and    --
-- a copy of the GCC Runtime Library Exception along with this program;     --
-- see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see    --
-- <http://www.gnu.org/licenses/>.                                          --
--                                                                          --
-- GNAT was originally developed  by the GNAT team at  New York University. --
-- Extensive contributions were provided by Ada Core Technologies Inc.      --
--                                                                          --
------------------------------------------------------------------------------

--  This package provides a generator of static minimal perfect hash functions.
--  To understand what a perfect hash function is, we define several notions.
--  These definitions are inspired from the following paper:

--    Zbigniew J. Czech, George Havas, and Bohdan S. Majewski ``An Optimal
--    Algorithm for Generating Minimal Perfect Hash Functions'', Information
--    Processing Letters, 43(1992) pp.257-264, Oct.1992

--  Let W be a set of m words. A hash function h is a function that maps the
--  set of words W into some given interval I of integers [0, k-1], where k is
--  an integer, usually k >= m. h (w) where w is a word in W computes an
--  address or an integer from I for the storage or the retrieval of that
--  item. The storage area used to store items is known as a hash table. Words
--  for which the same address is computed are called synonyms. Due to the
--  existence of synonyms a situation called collision may arise in which two
--  items w1 and w2 have the same address. Several schemes for resolving
--  collisions are known. A perfect hash function is an injection from the word
--  set W to the integer interval I with k >= m.  If k = m, then h is a minimal
--  perfect hash function. A hash function is order preserving if it puts
--  entries into the hash table in a prespecified order.

--  A minimal perfect hash function is defined by two properties:

--    Since no collisions occur each item can be retrieved from the table in
--    *one* probe. This represents the "perfect" property.

--    The hash table size corresponds to the exact size of W and *no larger*.
--    This represents the "minimal" property.

--  The functions generated by this package require the words to be known in
--  advance (they are "static" hash functions). The hash functions are also
--  order preserving. If w2 is inserted after w1 in the generator, then h (w1)
--  < h (w2). These hashing functions are convenient for use with realtime
--  applications.

package System.Perfect_Hash_Generators is

   type Optimization is (Memory_Space, CPU_Time);
   --  Optimize either the memory space or the execution time. Note: in
   --  practice, the optimization mode has little effect on speed. The tables
   --  are somewhat smaller with Memory_Space.

   Verbose : Boolean := False;
   --  Output the status of the algorithm. For instance, the tables, the random
   --  graph (edges, vertices) and selected char positions are output between
   --  two iterations.

   procedure Initialize
     (Seed  : Natural;
      V     : Positive;
      Optim : Optimization;
      Tries : Positive);
   --  Initialize the generator and its internal structures. Set the number of
   --  vertices in the random graphs. This value has to be greater than twice
   --  the number of keys in order for the algorithm to succeed. The word set
   --  is not modified (in particular when it is already set). For instance, it
   --  is possible to run several times the generator with different settings
   --  on the same words.
   --
   --  A classical way of doing is to Insert all the words and then to invoke
   --  Initialize and Compute. If this fails to find a perfect hash function,
   --  invoke Initialize again with other configuration parameters (probably
   --  with a greater number of vertices). Once successful, invoke Define and
   --  Value, and then Finalize.

   procedure Finalize;
   --  Deallocate the internal structures and the words table

   procedure Insert (Value : String);
   --  Insert a new word into the table. ASCII.NUL characters are not allowed.

   Too_Many_Tries : exception;
   --  Raised after Tries unsuccessful runs

   procedure Compute (Position : String);
   --  Compute the hash function. Position allows the definition of selection
   --  of character positions used in the word hash function. Positions can be
   --  separated by commas and ranges like x-y may be used. Character '$'
   --  represents the final character of a word. With an empty position, the
   --  generator automatically produces positions to reduce the memory usage.
   --  Raise Too_Many_Tries if the algorithm does not succeed within Tries
   --  attempts (see Initialize).

   --  The procedure Define returns the lengths of an internal table and its
   --  item type size. The function Value returns the value of each item in
   --  the table. Together they can be used to retrieve the parameters of the
   --  hash function which has been computed by a call to Compute.

   --  The hash function has the following form:

   --             h (w) = (g (f1 (w)) + g (f2 (w))) mod m

   --  G is a function based on a graph table [0,n-1] -> [0,m-1]. m is the
   --  number of keys. n is an internally computed value and it can be obtained
   --  as the length of vector G.

   --  F1 and F2 are two functions based on two function tables T1 and T2.
   --  Their definition depends on the chosen optimization mode.

   --  Only some character positions are used in the words because they are
   --  significant. They are listed in a character position table (P in the
   --  pseudo-code below). For instance, in {"jan", "feb", "mar", "apr", "jun",
   --  "jul", "aug", "sep", "oct", "nov", "dec"}, only positions 2 and 3 are
   --  significant (the first character can be ignored). In this example, P =
   --  {2, 3}

   --  When Optimization is CPU_Time, the first dimension of T1 and T2
   --  corresponds to the character position in the word and the second to the
   --  character set. As all the character set is not used, we define a used
   --  character table which associates a distinct index to each used character
   --  (unused characters are mapped to zero). In this case, the second
   --  dimension of T1 and T2 is reduced to the used character set (C in the
   --  pseudo-code below). Therefore, the hash function has the following:

   --    function Hash (S : String) return Natural is
   --       F      : constant Natural := S'First - 1;
   --       L      : constant Natural := S'Length;
   --       F1, F2 : Natural := 0;
   --       J      : <t>;

   --    begin
   --       for K in P'Range loop
   --          exit when L < P (K);
   --          J  := C (S (P (K) + F));
   --          F1 := (F1 + Natural (T1 (K, J))) mod <n>;
   --          F2 := (F2 + Natural (T2 (K, J))) mod <n>;
   --       end loop;

   --       return (Natural (G (F1)) + Natural (G (F2))) mod <m>;
   --    end Hash;

   --  When Optimization is Memory_Space, the first dimension of T1 and T2
   --  corresponds to the character position in the word and the second
   --  dimension is ignored. T1 and T2 are no longer matrices but vectors.
   --  Therefore, the used character table is not available. The hash function
   --  has the following form:

   --     function Hash (S : String) return Natural is
   --        F      : constant Natural := S'First - 1;
   --        L      : constant Natural := S'Length;
   --        F1, F2 : Natural := 0;
   --        J      : <t>;

   --     begin
   --        for K in P'Range loop
   --           exit when L < P (K);
   --           J  := Character'Pos (S (P (K) + F));
   --           F1 := (F1 + Natural (T1 (K) * J)) mod <n>;
   --           F2 := (F2 + Natural (T2 (K) * J)) mod <n>;
   --        end loop;

   --        return (Natural (G (F1)) + Natural (G (F2))) mod <m>;
   --     end Hash;

   type Table_Name is
     (Character_Position,
      Used_Character_Set,
      Function_Table_1,
      Function_Table_2,
      Graph_Table);

   procedure Define
     (Name      : Table_Name;
      Item_Size : out Natural;
      Length_1  : out Natural;
      Length_2  : out Natural);
   --  Return the definition of the table Name. This includes the length of
   --  dimensions 1 and 2 and the size of an unsigned integer item. When
   --  Length_2 is zero, the table has only one dimension. All the ranges
   --  start from zero.

   function Value
     (Name : Table_Name;
      J    : Natural;
      K    : Natural := 0) return Natural;
   --  Return the value of the component (J, K) of the table Name. When the
   --  table has only one dimension, K is ignored.

end System.Perfect_Hash_Generators;
