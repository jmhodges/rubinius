require File.dirname(__FILE__) + '/../spec_helper'

describe "A For node" do
  relates <<-ruby do
      for o in ary do
        puts(o)
      end
    ruby

    parse do
      [:for,
       [:call, nil, :ary, [:arglist]],
       [:lasgn, :o],
       [:call, nil, :puts, [:arglist, [:lvar, :o]]]]
    end

    compile do |g|
      g.push :self
      g.send :ary, 0, true

      in_block_send :each, 1.0, 0, false, 1 do |d|
        d.push :self
        d.push_local 0
        d.send :puts, 1, true
      end
    end
  end

  relates <<-ruby do
      for i in (0..max) do
        # do nothing
      end
    ruby

    parse do
      [:for, [:dot2, [:lit, 0], [:call, nil, :max, [:arglist]]], [:lasgn, :i]]
    end

    compile do |g|
      g.push_cpath_top
      g.find_const :Range
      g.push 0
      g.push :self
      g.send :max, 0, true
      g.send :new, 2

      in_block_send :each, 1.0, 0, false, 1 do |d|
        d.push :nil
      end
    end
  end

  relates <<-ruby do
      for a, b in x do
        5
      end
    ruby

    parse do
      [:for,
       [:call, nil, :x, [:arglist]],
       [:masgn, [:array, [:lasgn, :a], [:lasgn, :b]]],
       [:lit, 5]]
    end

    compile do |g|
      iter = description do |d|
        d.cast_for_multi_block_arg
        d.cast_array
        d.shift_array
        d.set_local 0
        d.pop
        d.shift_array
        d.set_local 1
        d.pop
        d.pop
        d.push_modifiers
        d.new_label.set!
        d.push 5
        d.pop_modifiers
        d.ret
      end

      g.passed_block(2) do
        g.push :self
        g.send :x, 0, true
        g.create_block iter
        g.send_with_block :each, 0, false
      end
    end
  end
end