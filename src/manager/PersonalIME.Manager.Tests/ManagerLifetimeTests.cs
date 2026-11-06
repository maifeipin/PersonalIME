using System.IO.Pipes;
using PersonalIME.Manager.Ipc;
using PersonalIME.Manager.ViewModels;

namespace PersonalIME.Manager.Tests;

[TestClass]
public class ManagerLifetimeTests
{
    [TestMethod]
    public async Task TestDisposingManagerCancelsReceivedButUnansweredRequest()
    {
        string name = $"PersonalIME_Lifetime_{Guid.NewGuid():N}";
        await using var server = new NamedPipeServerStream(name, PipeDirection.InOut, 1,
            PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        var connected = server.WaitForConnectionAsync(deadline.Token);
        var client = new AdminIpcClient(name);
        using var vm = new MainViewModel(client);
        var request = client.GetStatusAsync();
        await connected;
        var headerBytes = new byte[AdminHeader.HeaderSize];
        await server.ReadExactlyAsync(headerBytes, deadline.Token);
        var header = AdminHeader.ReadFrom(headerBytes);
        var payload = new byte[header.PayloadLength];
        await server.ReadExactlyAsync(payload, deadline.Token);
        Assert.IsFalse(request.IsCompleted, "Service has received the request but sent no response");

        vm.Dispose();
        await Assert.ThrowsAsync<OperationCanceledException>(async () =>
            await request.WaitAsync(TimeSpan.FromSeconds(1)));
        Assert.IsFalse(client.IsConnected);
        // Disposal must close the transport, not merely update IsDisposed.
        var probe = new byte[1];
        Assert.AreEqual(0, await server.ReadAsync(probe, deadline.Token));
    }

    [TestMethod]
    public async Task TestDisposedManagerDoesNotStartAnotherPipeRequest()
    {
        var client = new AdminIpcClient($"PersonalIME_Missing_{Guid.NewGuid():N}");
        var vm = new MainViewModel(client);
        vm.Dispose();
        vm.Dispose();
        await Assert.ThrowsAsync<OperationCanceledException>(async () =>
            await client.GetStatusAsync().WaitAsync(TimeSpan.FromSeconds(1)));
    }
}
