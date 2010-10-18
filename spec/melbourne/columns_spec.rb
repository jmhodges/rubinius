require File.expand_path('../../spec_helper', __FILE__)

describe "The Melbourne parser" do
  it "puts a column on alias" do
    parse("alias :foo :bar").column.should eql(1)
  end

  it "ignores leading whitespace" do
    parse("  alias :foo :bar").column.should eql(3)
    parse("\nalias :foo :bar").column.should eql(1)
    parse("\n alias :foo :bar").column.should eql(2)
    parse(" \nalias :foo :bar").column.should eql(1)
    parse(" \n alias :foo :bar").column.should eql(2)
    parse(" \t alias :foo :bar").column.should eql(4)
  end

  it "puts a column on self when it stands alone" do
    parse("self").column.should eql(1)
  end

  it "correctly sets the column on self after a method invocation" do
    parse("foo; self").array[1].column.should eql(6)
  end

  def parse(rb_str)
    Rubinius::Melbourne.parse_string(rb_str)
  end
end
