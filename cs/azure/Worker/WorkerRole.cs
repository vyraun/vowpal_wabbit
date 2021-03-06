using Microsoft.ApplicationInsights;
using Microsoft.ApplicationInsights.DataContracts;
using Microsoft.ApplicationInsights.Extensibility;
using Microsoft.Azure;
using Microsoft.Owin.Hosting;
using Microsoft.Practices.Unity;
using Microsoft.WindowsAzure.ServiceRuntime;
using Owin;
using System;
using System.Collections.Generic;
using System.Net;
using System.Threading;
using System.Web.Http;
using System.Web.Http.ExceptionHandling;
using Unity.WebApi;
using VowpalWabbit.Azure.Trainer;

namespace VowpalWabbit.Azure.Worker
{
    public class WorkerRole : RoleEntryPoint
    {
        private readonly ManualResetEventSlim stopEvent;
        private IDisposable webApp;
        private TelemetryClient telemetry;
        private OnlineTrainerSettingsWatcher settingsWatcher;
        private LearnEventProcessorHost trainProcesserHost;

        public WorkerRole()
        {
            this.stopEvent = new ManualResetEventSlim();
        }

        public override bool OnStart()
        {
            // Set the maximum number of concurrent connections
            ServicePointManager.DefaultConnectionLimit = 128;

            // For information on handling configuration changes
            // see the MSDN topic at http://go.microsoft.com/fwlink/?LinkId=166357.

            bool result = base.OnStart();

            TelemetryConfiguration.Active.InstrumentationKey = CloudConfigurationManager.GetSetting("APPINSIGHTS_INSTRUMENTATIONKEY");

            // TODO: disable
            // TelemetryConfiguration.Active.TelemetryChannel.DeveloperMode = true;
            this.telemetry = new TelemetryClient();

            this.telemetry.TrackTrace("WorkerRole starting", SeverityLevel.Information);

            try
            {
                this.trainProcesserHost = new LearnEventProcessorHost();
                this.settingsWatcher = new OnlineTrainerSettingsWatcher(this.trainProcesserHost);

                this.StartRESTAdminEndpoint();
            }
            catch (Exception e)
            {
                this.telemetry.TrackException(e);
                // still start to give AppInsights a chance to log
            }

            return result;
        }

        public override void Run()
        {
            try
            {
                // wait for OnStop
                this.stopEvent.Wait();
            }
            catch (Exception e)
            {
                this.telemetry.TrackException(e);
            }
        }

        private void StartRESTAdminEndpoint()
        {
            // setup REST endpoint
            var endpoint = RoleEnvironment.CurrentRoleInstance.InstanceEndpoints["OnlineTrainer"];
            string baseUri = String.Format("{0}://{1}",
                endpoint.Protocol, endpoint.IPEndpoint);

            this.webApp = WebApp.Start(baseUri, app =>
            {
                var container = new UnityContainer();
                
                // Register controller
                container.RegisterType<ResetController>();
                container.RegisterType<CheckpointController>();

                // Register interface
                container.RegisterInstance(typeof(LearnEventProcessorHost), this.trainProcesserHost);

                var config = new HttpConfiguration();
                config.DependencyResolver = new UnityDependencyResolver(container);
                config.Routes.MapHttpRoute(
                    "Default",
                    "{controller}/{id}",
                    new { id = RouteParameter.Optional });

                // config.Services.Add(typeof(IExceptionLogger), new AiWebApiExceptionLogger());

                app.UseWebApi(config);
            });
        }

        public override void OnStop()
        {
            this.telemetry.TrackTrace("WorkerRole stopping", SeverityLevel.Information);

            try
            {
                this.stopEvent.Set();

                if (this.settingsWatcher != null)
                {
                    this.settingsWatcher.Dispose();
                    this.settingsWatcher = null;
                }

                if (this.trainProcesserHost != null)
                {
                    this.trainProcesserHost.Dispose();
                    this.trainProcesserHost = null;
                }

                if (this.webApp != null)
                {
                    this.webApp.Dispose();
                    this.webApp = null;
                }

                base.OnStop();
            }
            catch (Exception e)
            {
                this.telemetry.TrackException(e);
            }

            this.telemetry.TrackTrace("WorkerRole stopped", SeverityLevel.Information);
        }
    }
}
