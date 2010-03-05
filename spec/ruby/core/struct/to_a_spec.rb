require File.expand_path('../../../spec_helper', __FILE__)
require File.expand_path('../fixtures/classes', __FILE__)

describe "Struct#to_a" do
  it "returns the values for this instance as an array" do
    Struct::Car.new('Geo', 'Metro', 1995).to_a.should == ['Geo', 'Metro', 1995]
    Struct::Car.new('Ford').to_a.should == ['Ford', nil, nil]
  end
end